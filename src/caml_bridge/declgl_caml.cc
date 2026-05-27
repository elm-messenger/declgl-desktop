// declgl_caml.cc — OCaml↔C++ bridge for the desktop ml_regl backend.
//
// This file provides the C symbols that [lib/backend/desktop/regl_desktop.ml]
// declares as `external`:
//
//     external declgl_ship_backend_cmd : bytes -> unit
//     external declgl_ship_audio_cmd   : bytes -> unit
//
// There is intentionally NO separate `declgl_run_main` entry point. The
// run loop is triggered by the protocol-level [StartRegl] command — the
// same way the JS backend's `start()` is what schedules
// `requestAnimationFrame(step)`. So the topology is:
//
//   1. OCaml owns process & main thread (provided by the OCaml runtime's
//      C `main`).
//   2. User OCaml code's top-level evaluates
//          let _ = Regl_desktop.create_app init update view
//      which (i) Callback.registers the runtime closures and (ii) calls
//      [h.init ()], whose first action is to ship a BackendCommandBatch
//      that contains a [StartRegl] entry.
//   3. [declgl_ship_backend_cmd] decodes the batch, dispatches every
//      command in order, and remembers whether any of them was a
//      [StartRegl]. After the loop over commands finishes (so commands
//      after [StartRegl] in the same batch still run first, exactly like
//      the JS backend's synchronous loop), if [StartRegl] was seen we
//      open the SDL3 window + GL ctx and enter the per-frame loop right
//      here. The call returns only when the user closes the window.
//   4. Each frame calls back into OCaml via [caml_callback] for every
//      pending event, then for [update], then for [view]. All callbacks
//      run on the same OCaml-runtime thread, so no acquire/release dance
//      is required.

#define CAML_NAME_SPACE
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/memory.h>
#include <caml/mlvalues.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "log/log.h"
#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace
{

// One global engine for the bridge. The JS backend doesn't support
// multiple windows either.
std::unique_ptr<declgl::Engine> &engine_storage()
{
	static std::unique_ptr<declgl::Engine> g;
	return g;
}

declgl::Engine *engine()
{
	return engine_storage().get();
}

// Cached pointers to the OCaml callbacks. Resolved lazily when [StartRegl]
// is processed, by which time user-side Callback.registers have run.
struct Callbacks {
	const value *event = nullptr;
	const value *view = nullptr;
	const value *recv_regl_cmd_pb = nullptr;
	const value *recv_audio_msg_pb = nullptr;

	bool resolve()
	{
		event = caml_named_value("declgl_app_event");
		view = caml_named_value("declgl_app_view");
		recv_regl_cmd_pb =
			caml_named_value("declgl_app_recv_regl_cmd_pb");
		recv_audio_msg_pb =
			caml_named_value("declgl_app_recv_audio_msg_pb");
		if (!event || !view) {
			DECLGL_LOG_ERROR("missing required Callback.register: "
					 "event={} view={}",
					 (void *)event, (void *)view);
			return false;
		}
		return true;
	}
};

Callbacks &callbacks()
{
	static Callbacks cb;
	return cb;
}

bool &loop_running_flag()
{
	static bool running = false;
	return running;
}

// Set when a [QuitRegl] BackendCommand arrives. Checked at the top of
// [drive_one_frame]; the loop exits before the next OCaml update so
// the OCaml model never observes the post-quit frame. Cleared after
// the run loop exits so a QuitRegl later in the same startup batch as
// StartRegl can still stop the just-created loop immediately.
bool &quit_requested_flag()
{
	static bool q = false;
	return q;
}

// Per-frame pacing target. interval_ms <= 0 means "auto" (vsync); >0
// means manual ms target — the bridge sleeps after SwapWindow until
// the frame budget is consumed. Mutated by [BackendCommand::kConfigRegl]
// inside [dispatch_batch]; read by [drive_one_frame].
struct PacingConfig {
	double interval_ms = -1.0; // -1 == auto / vsync
};
PacingConfig &pacing_config()
{
	static PacingConfig c;
	return c;
}

// Mouse events from SDL arrive in window coordinates. OCaml code, however,
// works in the same logical coordinate system used by renderables: the
// StartRegl virtual canvas. Keep that virtual size here so the event bridge
// can rescale mouse positions after window resize / fullscreen / high-DPI
// presentation changes.
struct VirtualCanvasSize {
	double width = 0.0;
	double height = 0.0;
};
VirtualCanvasSize &virtual_canvas_size()
{
	static VirtualCanvasSize s;
	return s;
}

struct MousePoint {
	double x = 0.0;
	double y = 0.0;
};

MousePoint mouse_to_virtual(float window_x, float window_y)
{
	MousePoint out{ window_x, window_y };
	SDL_Window *window = engine() ? engine()->sdl_window() : nullptr;
	const VirtualCanvasSize virt = virtual_canvas_size();
	if (!window || virt.width <= 0.0 || virt.height <= 0.0)
		return out;

	int window_w = 0;
	int window_h = 0;
	SDL_GetWindowSize(window, &window_w, &window_h);
	if (window_w <= 0 || window_h <= 0)
		return out;

	out.x = static_cast<double>(window_x) * virt.width /
		static_cast<double>(window_w);
	out.y = static_cast<double>(window_y) * virt.height /
		static_cast<double>(window_h);
	return out;
}

// ---------------------------------------------------------------------------
// Profiling mode (DECLGL_PROFILE env var)
// ---------------------------------------------------------------------------
// When enabled, records per-frame latency for:
//   - events:  SDL event pump
//   - update:  OCaml update callback
//   - view:    OCaml view callback + proto decode
//   - render:  C++ render + GL calls
//   - swap:    SDL_GL_SwapWindow
// On shutdown, writes a CSV to declgl_profile.csv (or path from env).

struct ProfileSample {
	uint64_t frame; // frame number (0-based)
	uint64_t events_ns; // SDL event pump
	uint64_t update_ns; // OCaml update callback
	uint64_t view_ns; // OCaml view + proto decode
	uint64_t render_ns; // C++ render
	uint64_t swap_ns; // GL swap
};

struct ProfilingState {
	bool enabled = false;
	std::string output_path;
	std::vector<ProfileSample> samples;
	uint64_t frame_counter = 0;
};

ProfilingState &profiling()
{
	static ProfilingState s;
	return s;
}

void profiling_init()
{
	const char *env = std::getenv("DECLGL_PROFILE");
	if (!env || !*env) {
		return;
	}
	profiling().enabled = true;
	// Allow DECLGL_PROFILE=path/to/file.csv or just DECLGL_PROFILE=1
	if (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0) {
		profiling().output_path = "declgl_profile.csv";
	} else {
		profiling().output_path = env;
	}
	profiling().samples.reserve(65536);
}

void profiling_record(const ProfileSample &s)
{
	if (!profiling().enabled) {
		return;
	}
	profiling().samples.push_back(s);
}

void profiling_shutdown()
{
	ProfilingState &s = profiling();
	if (!s.enabled || s.samples.empty()) {
		return;
	}

	std::ofstream f(s.output_path);
	if (!f.is_open()) {
		DECLGL_LOG_ERROR("profiling: cannot open '{}' for write",
				 s.output_path);
		return;
	}

	// CSV header
	f << "frame,events_ns,update_ns,view_ns,render_ns,swap_ns\n";

	// Summary stats for convenience (computed while writing)
	uint64_t total_events = 0, total_update = 0, total_view = 0;
	uint64_t total_render = 0, total_swap = 0;

	for (const auto &sample : s.samples) {
		f << sample.frame << ',' << sample.events_ns << ','
		  << sample.update_ns << ',' << sample.view_ns << ','
		  << sample.render_ns << ',' << sample.swap_ns << '\n';

		total_events += sample.events_ns;
		total_update += sample.update_ns;
		total_view += sample.view_ns;
		total_render += sample.render_ns;
		total_swap += sample.swap_ns;
	}

	f.close();

	const size_t n = s.samples.size();
	const double to_ms = 1.0 / 1e6;
	DECLGL_LOG_INFO("profiling: wrote {} frames to '{}'\n"
			"  avg per-frame (ms): events={:.3f} update={:.3f} "
			"view={:.3f} render={:.3f} swap={:.3f}",
			n, s.output_path,
			static_cast<double>(total_events) / n * to_ms,
			static_cast<double>(total_update) / n * to_ms,
			static_cast<double>(total_view) / n * to_ms,
			static_cast<double>(total_render) / n * to_ms,
			static_cast<double>(total_swap) / n * to_ms);

	// Reset for potential re-run
	s.samples.clear();
	s.frame_counter = 0;
	s.enabled = false;
}

// ---------------------------------------------------------------------------

// Shared time origin: the SDL tick captured when the run loop first
// entered. Same value the bridge subtracts before it ships the per-frame
// [UpdateTick] event, so [now_ms] computed off this anchor lines up
// with the OCaml-side clock that StartSound / volume timeline points
// reference. Zero before the loop has started — audio commands shipped
// pre-StartRegl get [now_ms = 0], which still works because the
// AudioEngine compares to its own [frames_produced_] anchor.
Uint64 &bridge_start_ticks()
{
	static Uint64 t = 0;
	return t;
}

// SDL → protobuf event encoder. Returns true if `out` was filled.
bool sdl_event_to_pb(const SDL_Event &ev,
		     mlregl::transport::backend::Event *out)
{
	using mlregl::transport::backend::KeyboardEvent;
	using mlregl::transport::backend::MouseEvent;
	using mlregl::transport::backend::MouseMoveEvent;
	switch (ev.type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		MouseEvent m;
		const MousePoint p = mouse_to_virtual(ev.button.x, ev.button.y);
		m.set_button(static_cast<uint32_t>(ev.button.button));
		m.set_x(p.x);
		m.set_y(p.y);
		*out->mutable_mouse_down() = m;
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		MouseEvent m;
		const MousePoint p = mouse_to_virtual(ev.button.x, ev.button.y);
		m.set_button(static_cast<uint32_t>(ev.button.button));
		m.set_x(p.x);
		m.set_y(p.y);
		*out->mutable_mouse_up() = m;
		return true;
	}
	case SDL_EVENT_MOUSE_MOTION: {
		MouseMoveEvent m;
		const MousePoint p = mouse_to_virtual(ev.motion.x, ev.motion.y);
		m.set_x(p.x);
		m.set_y(p.y);
		*out->mutable_mouse_move() = m;
		return true;
	}
	case SDL_EVENT_KEY_DOWN: {
		KeyboardEvent k;
		const char *name = SDL_GetKeyName(ev.key.key);
		k.set_code(name ? name : "");
		*out->mutable_key_down() = k;
		return true;
	}
	case SDL_EVENT_KEY_UP: {
		KeyboardEvent k;
		const char *name = SDL_GetKeyName(ev.key.key);
		k.set_code(name ? name : "");
		*out->mutable_key_up() = k;
		return true;
	}
	default:
		return false;
	}
}

// Pump SDL events. Returns false if the loop should exit.
bool pump_events_and_dispatch(Callbacks &cb)
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_EVENT_QUIT)
			return false;
		if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
			return false;

		mlregl::transport::backend::Event pb;
		if (!sdl_event_to_pb(ev, &pb))
			continue;

		std::string out;
		if (!pb.SerializeToString(&out))
			continue;

		CAMLparam0();
		CAMLlocal1(v_bytes);
		v_bytes = caml_alloc_initialized_string(out.size(), out.data());
		caml_callback(*cb.event, v_bytes);
		CAMLdrop;
	}
	return true;
}

// One frame: tick → update → view → render → swap. Returns false to exit.
bool drive_one_frame(Callbacks &cb, SDL_Window *window, Uint64 start_ticks)
{
	if (quit_requested_flag())
		return false;

	ProfileSample sample{};
	sample.frame = profiling().frame_counter++;

	const Uint64 t0 = SDL_GetTicksNS();
	if (!pump_events_and_dispatch(cb))
		return false;
	const Uint64 t1 = SDL_GetTicksNS();
	sample.events_ns = t1 - t0;

	const double now_ms = static_cast<double>(SDL_GetTicks() - start_ticks);

	{
		// Tick is shipped as a proto Event (UpdateTick), exactly like
		// the JS backend now does. The OCaml runtime decodes it in
		// [h.event] and dispatches it through the same input pipeline
		// as DOM/SDL events.
		mlregl::transport::backend::Event tick_pb;
		tick_pb.mutable_update_tick()->set_ts(now_ms);
		std::string out;
		if (tick_pb.SerializeToString(&out)) {
			CAMLparam0();
			CAMLlocal1(v_bytes);
			v_bytes = caml_alloc_initialized_string(out.size(),
								out.data());
			caml_callback(*cb.event, v_bytes);
			CAMLdrop;
		} else {
			DECLGL_LOG_ERROR("update_tick: serialize failed");
		}
	}
	const Uint64 t2 = SDL_GetTicksNS();
	sample.update_ns = t2 - t1;

	// [update] above can ship a QuitRegl; honour it before we burn
	// view/render work on a frame whose result will be discarded.
	if (quit_requested_flag())
		return false;

	{
		CAMLparam0();
		CAMLlocal1(v_view);
		v_view = caml_callback(*cb.view, Val_unit);
		if (Is_block(v_view)) {
			// Some bytes
			value v_bytes = Field(v_view, 0);
			const char *data = (const char *)Bytes_val(v_bytes);
			const size_t len = caml_string_length(v_bytes);

			mlregl::transport::render::Renderable r;
			if (r.ParseFromArray(data, static_cast<int>(len))) {
				// static int frame_log = 0;
				// if ((frame_log++ % 60) == 0) {
				// 	DECLGL_LOG_INFO(
				// 		"declgl/bridge",
				// 		"view: Renderable kind=%d (frame %d)",
				// 		static_cast<int>(r.kind_case()),
				// 		frame_log - 1);
				// }
				// Hand off to the engine for the actual draw calls.
				const Uint64 t_view_end = SDL_GetTicksNS();
				sample.view_ns = t_view_end - t2;
				engine()->render(r);
				const Uint64 t_render_end = SDL_GetTicksNS();
				sample.render_ns = t_render_end - t_view_end;
			} else {
				DECLGL_LOG_ERROR("view: parse failed ({} B)",
						 len);
				const Uint64 t_view_end = SDL_GetTicksNS();
				sample.view_ns = t_view_end - t2;
				sample.render_ns = 0;
			}
		} else {
			const Uint64 t_view_end = SDL_GetTicksNS();
			sample.view_ns = t_view_end - t2;
			sample.render_ns = 0;
		}
		CAMLdrop;
	}

	SDL_GL_SwapWindow(window);
	const Uint64 t3 = SDL_GetTicksNS();
	// If we didn't take the render path above (e.g. no view bytes),
	// measure swap from end of view phase.
	if (sample.render_ns == 0 && sample.view_ns > 0) {
		sample.swap_ns = t3 - (t2 + sample.view_ns);
	} else {
		sample.swap_ns = t3 - (t2 + sample.view_ns + sample.render_ns);
	}

	profiling_record(sample);

	// Manual frame pacing. With interval_ms <= 0 the swap above is
	// already throttled by vsync (set in [init_window_and_gl]); with
	// interval_ms > 0 vsync was disabled when the config landed and
	// we sleep here to hit the requested ms budget.
	const double interval_ms = pacing_config().interval_ms;
	if (interval_ms > 0.0) {
		const Uint64 elapsed_ns = t3 - t0;
		const Uint64 target_ns =
			static_cast<Uint64>(interval_ms * 1.0e6);
		if (elapsed_ns < target_ns)
			SDL_DelayNS(target_ns - elapsed_ns);
	}
	return true;
}

// Open the window + GL ctx and run the per-frame loop. Returns when the
// user closes the window. Called from inside [declgl_ship_backend_cmd]
// AFTER the StartRegl-bearing batch has been fully dispatched to the
// engine, so the engine state already reflects the start config + any
// earlier commands in the batch.
//
// Precondition: [engine()->init_window_and_gl] has already returned
// successfully — we want it to run *during* batch dispatch (so later
// commands in the same batch, e.g. LoadTexture, see GL state) rather
// than after, so [dispatch_batch] handles that on first sight of
// StartRegl.
void enter_run_loop()
{
	if (loop_running_flag()) {
		DECLGL_LOG_WARN("StartRegl while loop running; ignoring");
		return;
	}

	Callbacks &cb = callbacks();
	if (!cb.resolve())
		return;

	SDL_Window *window = engine()->sdl_window();
	bridge_start_ticks() = SDL_GetTicks();
	const Uint64 start_ticks = bridge_start_ticks();

	loop_running_flag() = true;
	profiling_init();
	while (drive_one_frame(cb, window, start_ticks)) {
		// Loop body intentionally empty — drive_one_frame does it all.
	}
	profiling_shutdown();
	loop_running_flag() = false;
	quit_requested_flag() = false;

	engine()->shutdown();
}

// Process every command in the decoded batch. Returns true iff the
// batch contained a [StartRegl] AND the window+GL bring-up succeeded —
// the caller should then enter the run loop. StartRegl is dispatched
// in-order: we bring up the window the moment we see it so any later
// command in the same batch (e.g. LoadTexture) lands on a live GL ctx.
bool dispatch_batch(const mlregl::transport::backend::BackendCommandBatch &batch)
{
	using namespace mlregl::transport::backend;
	bool need_loop = false;
	for (const BackendCommand &cmd : batch.commands()) {
		switch (cmd.kind_case()) {
		case BackendCommand::kStartRegl:
			if (!loop_running_flag() && !need_loop) {
				if (engine()->init_window_and_gl(
					    cmd.start_regl())) {
					const auto &start = cmd.start_regl();
					virtual_canvas_size().width =
						start.virt_width();
					virtual_canvas_size().height =
						start.virt_height();
					if (virtual_canvas_size().width <=
						    0.0 ||
					    virtual_canvas_size().height <=
						    0.0) {
						int ww = 0, wh = 0;
						SDL_GetWindowSize(
							engine()->sdl_window(),
							&ww, &wh);
						if (virtual_canvas_size()
							    .width <= 0.0)
							virtual_canvas_size()
								.width = ww;
						if (virtual_canvas_size()
							    .height <= 0.0)
							virtual_canvas_size()
								.height = wh;
					}
					need_loop = true;
				} else {
					DECLGL_LOG_ERROR(
						"init_window_and_gl failed");
					return false;
				}
			} else {
				DECLGL_LOG_WARN("duplicate StartRegl ignored");
			}
			break;

		case BackendCommand::kQuitRegl:
			// Set the flag; the run loop's [drive_one_frame] sees
			// it on the next iteration and tears down. If QuitRegl
			// arrives before StartRegl (no window yet) we just
			// log — there's no loop to stop.
			if (loop_running_flag() || need_loop) {
				quit_requested_flag() = true;
				DECLGL_LOG_INFO("quit_regl: stopping run loop");
			} else {
				DECLGL_LOG_WARN(
					"quit_regl before StartRegl; ignoring");
			}
			break;

		case BackendCommand::kConfigRegl: {
			// ConfigRegl targets the bridge-owned window + per-
			// frame loop, so the bridge consumes it directly
			// instead of forwarding to the engine. By design,
			// configuring before StartRegl is meaningless (no
			// window, no loop), so we ignore it with a warning.
			if (!loop_running_flag() && !need_loop) {
				DECLGL_LOG_WARN(
					"config_regl before StartRegl; ignoring");
				break;
			}
			const auto &cr = cmd.config_regl();
			switch (cr.config_case()) {
			case ReglConfig::kIntervalMs: {
				const double ms = cr.interval_ms();
				pacing_config().interval_ms = ms;
				// Vsync would double-throttle a manual ms
				// target, so disable it for >0 and re-enable
				// adaptive vsync (with regular vsync as a
				// fallback) when returning to auto.
				if (ms > 0.0) {
					SDL_GL_SetSwapInterval(0);
					DECLGL_LOG_INFO(
						"config_regl interval_ms={} (manual pacing)",
						ms);
				} else {
					if (!SDL_GL_SetSwapInterval(-1))
						SDL_GL_SetSwapInterval(1);
					DECLGL_LOG_INFO(
						"config_regl interval_ms={} (vsync)",
						ms);
				}
				break;
			}
			case ReglConfig::kWindow: {
				SDL_Window *w = engine()->sdl_window();
				if (!w) {
					DECLGL_LOG_WARN(
						"config_regl(window) but no window; ignoring");
					break;
				}
				const auto &wc = cr.window();
				if (wc.has_fullscreen()) {
					SDL_SetWindowFullscreen(
						w, wc.fullscreen());
					DECLGL_LOG_INFO(
						"config_regl window.fullscreen={}",
						wc.fullscreen() ? 1 : 0);
				}
				if (wc.has_resizable()) {
					SDL_SetWindowResizable(w,
							       wc.resizable());
					DECLGL_LOG_INFO(
						"config_regl window.resizable={}",
						wc.resizable() ? 1 : 0);
				}
				break;
			}
			case ReglConfig::CONFIG_NOT_SET:
				DECLGL_LOG_WARN(
					"config_regl with no payload; ignoring");
				break;
			}
			break;
		}

		case BackendCommand::KIND_NOT_SET:
			break;
		default:
			// All other command kinds are forwarded to the engine for its
			// existing decode-and-log handling. Once M3+ lands the engine
			// gets real implementations.
			engine()->dispatch_backend_command(cmd);
			break;
		}
	}
	return need_loop;
}

} // namespace

// ---------------------------------------------------------------------------
// OCaml-callable primitives
// ---------------------------------------------------------------------------

extern "C" CAMLprim value declgl_ship_backend_cmd(value v_bytes)
{
	CAMLparam1(v_bytes);

	// Lazily create the engine on the very first ship — well before any
	// window/GL context exists. The engine just holds decoder state at
	// this point; window+GL come up only when StartRegl is processed.
	if (!engine()) {
		engine_storage() = std::make_unique<declgl::Engine>();

		// Wire the BackendEvent sink so the engine can post back to
		// OCaml (TextureLoaded, FontLoaded, ProgramCreated, ...). The
		// sink is engine-side opaque (`function<void(bytes,len)>`); we
		// resolve `recv_regl_cmd_pb` lazily inside the closure because
		// the user-side `Callback.register` may not have run yet at
		// engine-construction time.
		engine()->set_event_sink([](const uint8_t *bytes,
					    std::size_t len) {
			const value *recv =
				caml_named_value("declgl_app_recv_regl_cmd_pb");
			if (!recv) {
				DECLGL_LOG_ERROR(
					"BackendEvent dropped: "
					"callback 'declgl_app_recv_regl_cmd_pb' not registered");
				return;
			}
			CAMLparam0();
			CAMLlocal1(v_bytes);
			v_bytes = caml_alloc_initialized_string(
				len, reinterpret_cast<const char *>(bytes));
			caml_callback(*recv, v_bytes);
			CAMLdrop;
		});

		// Same shape as above but for AudioBackendEvent
		// (audio_context_ready / audio_load_success /
		// audio_load_failed). The OCaml side dispatches them
		// through a separate Callback.register named
		// 'declgl_app_recv_audio_msg_pb'.
		engine()->set_audio_event_sink([](const uint8_t *bytes,
						  std::size_t len) {
			const value *recv = caml_named_value(
				"declgl_app_recv_audio_msg_pb");
			if (!recv) {
				DECLGL_LOG_ERROR(
					"AudioBackendEvent dropped: "
					"callback 'declgl_app_recv_audio_msg_pb' not registered");
				return;
			}
			CAMLparam0();
			CAMLlocal1(v_bytes);
			v_bytes = caml_alloc_initialized_string(
				len, reinterpret_cast<const char *>(bytes));
			caml_callback(*recv, v_bytes);
			CAMLdrop;
		});
	}

	const uint8_t *p =
		reinterpret_cast<const uint8_t *>(Bytes_val(v_bytes));
	const size_t n = caml_string_length(v_bytes);

	mlregl::transport::backend::BackendCommandBatch batch;
	if (!batch.ParseFromArray(p, static_cast<int>(n))) {
		DECLGL_LOG_ERROR("BackendCommandBatch parse failed ({} B)", n);
		CAMLreturn(Val_unit);
	}

	const bool started = dispatch_batch(batch);
	if (started && !loop_running_flag()) {
		// We're (re-)entering the loop. This call only returns when the
		// user closes the window. Inside the loop, OCaml callbacks may
		// themselves call back into declgl_ship_backend_cmd (e.g. the
		// user's update returns more commands) — that's fine, dispatch
		// is reentrant since it doesn't touch the loop_running_flag
		// when no StartRegl is present.
		enter_run_loop();
	}

	CAMLreturn(Val_unit);
}

extern "C" CAMLprim value declgl_ship_audio_cmd(value v_bytes)
{
	CAMLparam1(v_bytes);

	if (!engine()) {
		engine_storage() = std::make_unique<declgl::Engine>();
	}

	const uint8_t *p =
		reinterpret_cast<const uint8_t *>(Bytes_val(v_bytes));
	const size_t n = caml_string_length(v_bytes);

	// Stamp the batch with the same ms clock OCaml's [update] is
	// driven on. If the run loop hasn't started yet we ship 0 — the
	// AudioEngine handles that fine (it'll just queue commands
	// against frame 0 of the playback timeline).
	const double now_ms = bridge_start_ticks() == 0 ?
				      0.0 :
				      static_cast<double>(SDL_GetTicks() -
							  bridge_start_ticks());
	engine()->exec_audio_cmd(p, n, now_ms);

	CAMLreturn(Val_unit);
}
