// runtime.cc — transport-agnostic SDL/GL run loop for ml_regl desktop.
//
// All the per-frame machinery (SDL event pump, dispatch_batch over
// BackendCommand, frame pacing, profiling, etc.) lives here. The host
// (OCaml caml_bridge or future gRPC player) hands a LoopHooks subclass
// to a Runtime instance and never touches SDL or GL directly.

#include "runtime/runtime.h"

#include "runtime/loop_hooks.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "log/log.h"
#include "renderer/render_context.h"
#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl
{

namespace
{

// ---------------------------------------------------------------------------
// Profiling mode (DECLGL_PROFILE env var)
// ---------------------------------------------------------------------------
// When enabled, records per-frame latency for:
//   - events:  SDL event pump
//   - update:  host update callback (UpdateTick deliver_event)
//   - view:    host view callback + proto decode
//   - render:  C++ render + GL calls
//   - swap:    SDL_GL_SwapWindow
// On shutdown, writes a CSV to declgl_profile.csv (or path from env).

struct ProfileSample {
	uint64_t frame;
	uint64_t events_ns;
	uint64_t update_ns;
	uint64_t view_ns;
	uint64_t render_ns;
	uint64_t swap_ns;
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

	f << "frame,events_ns,update_ns,view_ns,render_ns,swap_ns\n";

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

	s.samples.clear();
	s.frame_counter = 0;
	s.enabled = false;
}

struct MousePoint {
	double x = 0.0;
	double y = 0.0;
};

} // namespace

// ---------------------------------------------------------------------------
// Runtime::Impl — process-wide state, all in one place. Singleton-ish via
// the host: there's typically one Runtime per process (matching the JS
// backend's "one window" assumption). Multiple Runtimes would each open
// their own engine and trade off SDL state, but that's untested.
// ---------------------------------------------------------------------------
struct Runtime::Impl {
	explicit Impl(LoopHooks &hooks)
		: hooks_(hooks)
	{
	}

	LoopHooks &hooks_;

	std::unique_ptr<declgl::Engine> engine_;
	bool engine_sinks_installed_ = false;

	bool loop_running_ = false;
	// Set when a [QuitRegl] BackendCommand arrives. Checked at the top
	// of drive_one_frame; the loop exits before the next host update so
	// the host model never observes the post-quit frame. Cleared after
	// the run loop exits so a QuitRegl later in the same startup batch
	// as StartRegl can still stop the just-created loop immediately.
	bool quit_requested_ = false;

	// Per-frame pacing target. interval_ms <= 0 means "auto" (vsync); >0
	// means manual ms target — we sleep after SwapWindow until the
	// frame budget is consumed.
	double pacing_interval_ms_ = -1.0;

	// Maximum ready asset jobs to finish at the start of each frame.
	// 0 means unlimited.
	std::size_t max_assets_per_frame_ = 4;

	// Mouse events from SDL arrive in window coordinates. Host code,
	// however, works in the same logical coordinate system used by
	// renderables: the StartRegl virtual canvas. Keep that virtual size
	// here so the event bridge can rescale mouse positions after window
	// resize / fullscreen / high-DPI presentation changes.
	double virt_width_ = 0.0;
	double virt_height_ = 0.0;

	// Shared time origin captured when the run loop first entered.
	// Audio commands timestamp themselves relative to this so the
	// AudioEngine sees the same clock as the per-frame UpdateTick.
	Uint64 start_ticks_ = 0;

	void ensure_engine();
	void install_engine_sinks();

	MousePoint mouse_to_virtual(float window_x, float window_y);
	bool sdl_event_to_pb(const SDL_Event &ev,
			     mlregl::transport::backend::Event *out);

	bool pump_events();
	bool drive_one_frame();
	void apply_pulled_commands();

	bool dispatch_batch(
		const mlregl::transport::backend::BackendCommandBatch &batch);
};

// ---------------------------------------------------------------------------

void Runtime::Impl::ensure_engine()
{
	if (engine_)
		return;
	engine_ = std::make_unique<declgl::Engine>();
	install_engine_sinks();
}

void Runtime::Impl::install_engine_sinks()
{
	if (engine_sinks_installed_)
		return;
	if (!engine_)
		return;
	engine_->set_event_sink([this](const uint8_t *bytes,
				       std::size_t len) {
		hooks_.on_backend_event(bytes, len);
	});
	engine_->set_audio_event_sink([this](const uint8_t *bytes,
					     std::size_t len) {
		hooks_.on_audio_event(bytes, len);
	});
	engine_sinks_installed_ = true;
}

MousePoint Runtime::Impl::mouse_to_virtual(float window_x, float window_y)
{
	MousePoint out{ window_x, window_y };
	SDL_Window *window = engine_ ? engine_->sdl_window() : nullptr;
	if (!window || virt_width_ <= 0.0 || virt_height_ <= 0.0)
		return out;

	int window_w = 0;
	int window_h = 0;
	SDL_GetWindowSize(window, &window_w, &window_h);
	if (window_w <= 0 || window_h <= 0)
		return out;

	// Mirror the renderer's letterbox/pillarbox fit so the host app
	// sees the same virtual coords no matter the window aspect. SDL
	// mouse events are in logical units (matching SDL_GetWindowSize),
	// so we recompute the fit here in logical units rather than
	// reusing the engine's pixel-space fit_rect (the aspect is
	// identical, but the magnitudes differ on hi-DPI displays). Coords
	// are clamped to [0, virt_w/h] so clicks landing on a bar still
	// report a valid in-canvas position.
	int off_x = 0, off_y = 0, fit_w = 0, fit_h = 0;
	declgl::compute_fit_rect(window_w, window_h, virt_width_, virt_height_,
				 off_x, off_y, fit_w, fit_h);
	if (fit_w <= 0 || fit_h <= 0)
		return out;

	double vx = (static_cast<double>(window_x) - off_x) * virt_width_ /
		    static_cast<double>(fit_w);
	double vy = (static_cast<double>(window_y) - off_y) * virt_height_ /
		    static_cast<double>(fit_h);
	if (vx < 0.0) vx = 0.0;
	if (vy < 0.0) vy = 0.0;
	if (vx > virt_width_) vx = virt_width_;
	if (vy > virt_height_) vy = virt_height_;
	out.x = vx;
	out.y = vy;
	return out;
}

bool Runtime::Impl::sdl_event_to_pb(const SDL_Event &ev,
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
		DECLGL_LOG_TRACE("mouse_down: button={} virt=({:.2f},{:.2f})",
				 ev.button.button, p.x, p.y);
		*out->mutable_mouse_down() = m;
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		MouseEvent m;
		const MousePoint p = mouse_to_virtual(ev.button.x, ev.button.y);
		m.set_button(static_cast<uint32_t>(ev.button.button));
		m.set_x(p.x);
		m.set_y(p.y);
		DECLGL_LOG_TRACE("mouse_up: button={} virt=({:.2f},{:.2f})",
				 ev.button.button, p.x, p.y);
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
		DECLGL_LOG_TRACE("key_down: {}", name ? name : "");
		*out->mutable_key_down() = k;
		return true;
	}
	case SDL_EVENT_KEY_UP: {
		KeyboardEvent k;
		const char *name = SDL_GetKeyName(ev.key.key);
		k.set_code(name ? name : "");
		DECLGL_LOG_TRACE("key_up: {}", name ? name : "");
		*out->mutable_key_up() = k;
		return true;
	}
	default:
		return false;
	}
}

bool Runtime::Impl::pump_events()
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

		hooks_.deliver_event(reinterpret_cast<const uint8_t *>(
					     out.data()),
				     out.size());
	}
	return true;
}

void Runtime::Impl::apply_pulled_commands()
{
	auto blobs = hooks_.pull_commands();
	if (blobs.empty())
		return;
	for (const auto &blob : blobs) {
		mlregl::transport::backend::BackendCommandBatch batch;
		if (!batch.ParseFromArray(blob.data(),
					  static_cast<int>(blob.size()))) {
			DECLGL_LOG_ERROR(
				"pull_commands: BackendCommandBatch parse "
				"failed ({} B)",
				blob.size());
			continue;
		}
		dispatch_batch(batch);
	}
}

bool Runtime::Impl::drive_one_frame()
{
	if (quit_requested_)
		return false;

	hooks_.before_frame();
	apply_pulled_commands();
	if (quit_requested_)
		return false;

	ProfileSample sample{};
	sample.frame = profiling().frame_counter++;

	const Uint64 t0 = SDL_GetTicksNS();
	if (!pump_events())
		return false;
	const Uint64 t1 = SDL_GetTicksNS();
	sample.events_ns = t1 - t0;

	const double now_ms =
		static_cast<double>(SDL_GetTicks() - start_ticks_);

	{
		// Tick is shipped as a proto Event (UpdateTick), exactly like
		// the JS backend now does. The host runtime decodes it in its
		// event handler and dispatches it through the same input
		// pipeline as DOM/SDL events.
		mlregl::transport::backend::Event tick_pb;
		tick_pb.mutable_update_tick()->set_ts(now_ms);
		std::string out;
		if (tick_pb.SerializeToString(&out)) {
			hooks_.deliver_event(reinterpret_cast<const uint8_t *>(
						     out.data()),
					     out.size());
		} else {
			DECLGL_LOG_ERROR("update_tick: serialize failed");
		}
	}
	const Uint64 t2 = SDL_GetTicksNS();
	sample.update_ns = t2 - t1;

	// The host's deliver_event path (above) may have shipped a
	// QuitRegl back via dispatch(); honour it before we burn view/
	// render work on a frame whose result will be discarded.
	if (quit_requested_)
		return false;

	{
		auto view_bytes = hooks_.pull_view();
		if (view_bytes && !view_bytes->empty()) {
			mlregl::transport::render::Renderable r;
			if (r.ParseFromArray(
				    view_bytes->data(),
				    static_cast<int>(view_bytes->size()))) {
				const Uint64 t_view_end = SDL_GetTicksNS();
				sample.view_ns = t_view_end - t2;
				engine_->render(r, max_assets_per_frame_);
				const Uint64 t_render_end = SDL_GetTicksNS();
				sample.render_ns = t_render_end - t_view_end;
			} else {
				DECLGL_LOG_ERROR("view: parse failed ({} B)",
						 view_bytes->size());
				const Uint64 t_view_end = SDL_GetTicksNS();
				sample.view_ns = t_view_end - t2;
				sample.render_ns = 0;
			}
		} else {
			const Uint64 t_view_end = SDL_GetTicksNS();
			sample.view_ns = t_view_end - t2;
			sample.render_ns = 0;
		}
	}

	SDL_Window *window = engine_ ? engine_->sdl_window() : nullptr;
	if (window)
		SDL_GL_SwapWindow(window);
	const Uint64 t3 = SDL_GetTicksNS();
	if (sample.render_ns == 0 && sample.view_ns > 0) {
		sample.swap_ns = t3 - (t2 + sample.view_ns);
	} else {
		sample.swap_ns = t3 - (t2 + sample.view_ns + sample.render_ns);
	}

	profiling_record(sample);

	// Manual frame pacing. With pacing_interval_ms_ <= 0 the swap
	// above is already throttled by vsync (set in init_window_and_gl);
	// with > 0 vsync was disabled when the config landed and we sleep
	// here to hit the requested ms budget.
	if (pacing_interval_ms_ > 0.0) {
		const Uint64 elapsed_ns = t3 - t0;
		const Uint64 target_ns =
			static_cast<Uint64>(pacing_interval_ms_ * 1.0e6);
		if (elapsed_ns < target_ns)
			SDL_DelayNS(target_ns - elapsed_ns);
	}

	hooks_.after_frame();
	return true;
}

bool Runtime::Impl::dispatch_batch(
	const mlregl::transport::backend::BackendCommandBatch &batch)
{
	using namespace mlregl::transport::backend;
	bool need_loop = false;
	for (const BackendCommand &cmd : batch.commands()) {
		switch (cmd.kind_case()) {
		case BackendCommand::kStartRegl:
			if (!loop_running_ && !need_loop) {
				if (engine_->init_window_and_gl(
					    cmd.start_regl())) {
					const auto &start = cmd.start_regl();
					virt_width_ = start.virt_width();
					virt_height_ = start.virt_height();
					if (virt_width_ <= 0.0 ||
					    virt_height_ <= 0.0) {
						int ww = 0, wh = 0;
						SDL_GetWindowSize(
							engine_->sdl_window(),
							&ww, &wh);
						if (virt_width_ <= 0.0)
							virt_width_ = ww;
						if (virt_height_ <= 0.0)
							virt_height_ = wh;
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
			// Set the flag; the run loop's drive_one_frame sees
			// it on the next iteration and tears down. If
			// QuitRegl arrives before StartRegl (no window yet)
			// we just log — there's no loop to stop.
			if (loop_running_ || need_loop) {
				quit_requested_ = true;
				hooks_.on_quit();
				DECLGL_LOG_INFO("quit_regl: stopping run loop");
			} else {
				DECLGL_LOG_WARN(
					"quit_regl before StartRegl; ignoring");
			}
			break;

		case BackendCommand::kConfigRegl: {
			// ConfigRegl targets the runtime-owned window +
			// per-frame loop, so the runtime consumes it directly
			// instead of forwarding to the engine. By design,
			// configuring before StartRegl is meaningless (no
			// window, no loop), so we ignore it with a warning.
			if (!loop_running_ && !need_loop) {
				DECLGL_LOG_WARN(
					"config_regl before StartRegl; ignoring");
				break;
			}
			const auto &cr = cmd.config_regl();
			switch (cr.config_case()) {
			case ReglConfig::kIntervalMs: {
				const double ms = cr.interval_ms();
				pacing_interval_ms_ = ms;
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
				SDL_Window *w = engine_->sdl_window();
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
			case ReglConfig::kMaxAssetsPerFrame: {
				max_assets_per_frame_ =
					static_cast<std::size_t>(
						cr.max_assets_per_frame());
				DECLGL_LOG_INFO(
					"config_regl max_assets_per_frame={}",
					max_assets_per_frame_);
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
			engine_->dispatch_backend_command(cmd);
			break;
		}
	}
	return need_loop;
}

// ---------------------------------------------------------------------------
// Runtime public API
// ---------------------------------------------------------------------------

Runtime::Runtime(LoopHooks &hooks)
	: impl_(std::make_unique<Impl>(hooks))
{
}

Runtime::~Runtime() = default;

bool Runtime::is_loop_running() const
{
	return impl_->loop_running_;
}

Engine *Runtime::engine()
{
	return impl_->engine_.get();
}

bool Runtime::dispatch(const uint8_t *bytes, std::size_t len)
{
	impl_->ensure_engine();

	mlregl::transport::backend::BackendCommandBatch batch;
	if (!batch.ParseFromArray(bytes, static_cast<int>(len))) {
		DECLGL_LOG_ERROR("BackendCommandBatch parse failed ({} B)",
				 len);
		return false;
	}

	return impl_->dispatch_batch(batch);
}

void Runtime::dispatch_audio(const uint8_t *bytes, std::size_t len)
{
	impl_->ensure_engine();

	// Stamp the batch with the same ms clock the run loop is driven on.
	// If the loop hasn't started yet we ship 0 — the AudioEngine handles
	// that fine (it'll just queue commands against frame 0 of the
	// playback timeline).
	const double now_ms =
		impl_->start_ticks_ == 0 ?
			0.0 :
			static_cast<double>(SDL_GetTicks() -
					    impl_->start_ticks_);
	impl_->engine_->exec_audio_cmd(bytes, len, now_ms);
}

void Runtime::run()
{
	if (impl_->loop_running_) {
		DECLGL_LOG_WARN("run() called while loop running; ignoring");
		return;
	}
	if (!impl_->engine_ || !impl_->engine_->sdl_window()) {
		DECLGL_LOG_ERROR(
			"run() called before StartRegl brought up window; "
			"ignoring");
		return;
	}

	if (!impl_->hooks_.on_loop_enter())
		return;

	impl_->start_ticks_ = SDL_GetTicks();
	impl_->loop_running_ = true;
	profiling_init();
	while (impl_->drive_one_frame()) {
		// Body intentionally empty — drive_one_frame does it all.
	}
	profiling_shutdown();
	impl_->loop_running_ = false;
	impl_->quit_requested_ = false;

	impl_->hooks_.on_loop_exit();

	impl_->engine_->shutdown();
}

} // namespace declgl
