// engine.cc — Internal C++ engine implementation for the desktop backend.

#include "engine/engine.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "audio/audio_engine.h"
#include "gpu/fbo_pool.h"
#include "log/log.h"
#include "renderer/decl_program_registry.h"
#include "renderer/programs/dynamic_program.h"
#include "renderer/render_context.h"
#include "renderer/renderable_walker.h"
#include "resources/asset_loader.h"
#include "resources/font.h"
#include "resources/font_registry.h"
#include "resources/image_decoder.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"
#include "transport_audio.pb.h"
#include "transport_backend.pb.h"

namespace declgl
{

namespace
{

std::string &last_error_storage()
{
	static std::string s;
	return s;
}

const char *describe_min(mlregl::transport::backend::TextureMinOption m)
{
	using M = mlregl::transport::backend::TextureMinOption;
	switch (m) {
	case M::TEXTURE_MIN_OPTION_LINEAR:
		return "LINEAR";
	case M::TEXTURE_MIN_OPTION_NEAREST:
		return "NEAREST";
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST:
		return "N_MIP_N";
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST:
		return "L_MIP_N";
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR:
		return "N_MIP_L";
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR:
		return "L_MIP_L";
	default:
		return "?";
	}
}

const char *describe_mag(mlregl::transport::backend::TextureMagOption m)
{
	using M = mlregl::transport::backend::TextureMagOption;
	return m == M::TEXTURE_MAG_OPTION_NEAREST ? "NEAREST" : "LINEAR";
}

// Map proto-level filter enums → engine-side [TextureFilter].
TextureFilter to_filter_min(mlregl::transport::backend::TextureMinOption m)
{
	using M = mlregl::transport::backend::TextureMinOption;
	switch (m) {
	case M::TEXTURE_MIN_OPTION_NEAREST:
		return TextureFilter::Nearest;
	case M::TEXTURE_MIN_OPTION_LINEAR:
		return TextureFilter::Linear;
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST:
		return TextureFilter::NearestMipmapNearest;
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST:
		return TextureFilter::LinearMipmapNearest;
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR:
		return TextureFilter::NearestMipmapLinear;
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR:
		return TextureFilter::LinearMipmapLinear;
	default:
		return TextureFilter::Linear;
	}
}

TextureFilter to_filter_mag(mlregl::transport::backend::TextureMagOption m)
{
	using M = mlregl::transport::backend::TextureMagOption;
	return m == M::TEXTURE_MAG_OPTION_NEAREST ? TextureFilter::Nearest :
						    TextureFilter::Linear;
}

// True iff [m] is one of the four mipmap minification modes.
bool min_filter_uses_mipmaps(mlregl::transport::backend::TextureMinOption m)
{
	using M = mlregl::transport::backend::TextureMinOption;
	switch (m) {
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST:
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST:
	case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR:
	case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR:
		return true;
	default:
		return false;
	}
}

} // namespace

void set_error(std::string msg)
{
	last_error_storage() = std::move(msg);
}

const char *last_error()
{
	return last_error_storage().c_str();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() = default;
Engine::~Engine()
{
	shutdown();
}

void Engine::init_decoders_only()
{
	// Bring the async log backend up first so every other init
	// step can use declgl::log::*. Idempotent.
	declgl::log::init();

	// The async asset decode pipeline doesn't touch GL, so we bring it
	// up here — well before [init_window_and_gl] — so any LoadTexture /
	// LoadFont commands that arrive between [init_decoders_only] and
	// [StartRegl] get decoded in parallel with the rest of startup.
	// Their ready buffers sit in the loader's queue until [render()]
	// starts draining them.
	if (!loader_) {
		loader_ = std::make_unique<AssetLoader>();
	}
	if (!audio_) {
		audio_ = std::make_unique<AudioEngine>();
	}
	set_error("");
}

void Engine::set_audio_event_sink(EventSink sink)
{
	if (!audio_) {
		audio_ = std::make_unique<AudioEngine>();
	}
	audio_->set_event_sink(std::move(sink));
}

bool Engine::init_window_and_gl(
	const mlregl::transport::backend::StartRegl &start)
{
	if (window_) {
		// Idempotent on repeated StartRegl — bridge guards against this
		// too, but be defensive.
		return true;
	}

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
		set_error(std::string("SDL_Init: ") + SDL_GetError());
		return false;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
			    SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
			    SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	// StartRegl carries virt_width/virt_height as the logical/virtual
	// size; the desktop backend currently treats them 1:1 as window size.
	const int32_t w = start.virt_width() > 0 ?
				  static_cast<int32_t>(start.virt_width()) :
				  1280;
	const int32_t h = start.virt_height() > 0 ?
				  static_cast<int32_t>(start.virt_height()) :
				  720;

	// Default window flags. The optional WindowConfig sub-message
	// overrides each control: absent fields keep the default.
	bool want_resizable = true;
	bool want_fullscreen = false;
	if (start.has_window()) {
		const auto &wc = start.window();
		if (wc.has_resizable())
			want_resizable = wc.resizable();
		if (wc.has_fullscreen())
			want_fullscreen = wc.fullscreen();
	}
	SDL_WindowFlags wflags = SDL_WINDOW_OPENGL |
				 SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (want_resizable)
		wflags |= SDL_WINDOW_RESIZABLE;
	if (want_fullscreen)
		wflags |= SDL_WINDOW_FULLSCREEN;

	window_ = SDL_CreateWindow("declgl", w, h, wflags);
	if (!window_) {
		set_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
		SDL_Quit();
		return false;
	}

	gl_ctx_ = SDL_GL_CreateContext(window_);
	if (!gl_ctx_) {
		set_error(std::string("SDL_GL_CreateContext: ") +
			  SDL_GetError());
		SDL_DestroyWindow(window_);
		window_ = nullptr;
		SDL_Quit();
		return false;
	}

	if (!SDL_GL_MakeCurrent(window_, gl_ctx_)) {
		set_error(std::string("SDL_GL_MakeCurrent: ") + SDL_GetError());
		SDL_GL_DestroyContext(gl_ctx_);
		gl_ctx_ = nullptr;
		SDL_DestroyWindow(window_);
		window_ = nullptr;
		SDL_Quit();
		return false;
	}

	// Adaptive vsync if available, else regular vsync.
	if (!SDL_GL_SetSwapInterval(-1)) {
		SDL_GL_SetSwapInterval(1);
	}

	int gl_version = gladLoadGL(
		reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
	if (gl_version == 0) {
		set_error("gladLoadGL failed");
		shutdown();
		return false;
	}

	DECLGL_LOG_INFO(
		"GL {}.{}  vendor={}  renderer={}  glsl={}",
		GLAD_VERSION_MAJOR(gl_version), GLAD_VERSION_MINOR(gl_version),
		reinterpret_cast<const char *>(glGetString(GL_VENDOR)),
		reinterpret_cast<const char *>(glGetString(GL_RENDERER)),
		reinterpret_cast<const char *>(
			glGetString(GL_SHADING_LANGUAGE_VERSION)));

	if (start.has_builtin_programs()) {
		DECLGL_LOG_INFO("start: virt={}x{} fbo_num={} builtins={}",
				start.virt_width(), start.virt_height(),
				start.fbo_num(),
				start.builtin_programs().values_size());
	} else {
		DECLGL_LOG_INFO("start: virt={}x{} fbo_num={}",
				start.virt_width(), start.virt_height(),
				start.fbo_num());
	}

	// M3.B: spin up the declarative program registry, render context and walker.
	// These all need an active GL context, so we construct them here
	// rather than in [init_decoders_only].
	decl_programs_ = std::make_unique<DeclProgramRegistry>();
	textures_ = std::make_unique<TextureRegistry>();
	fonts_ = std::make_unique<FontRegistry>();
	fbos_ = std::make_unique<FboPool>();
	render_ctx_ = std::make_unique<RenderContext>();
	walker_ = std::make_unique<RenderableWalker>(*decl_programs_);
	render_ctx_->textures = textures_.get();
	render_ctx_->fonts = fonts_.get();
	render_ctx_->fbos = fbos_.get();

	render_ctx_->view_w = static_cast<float>(start.virt_width());
	render_ctx_->view_h = static_cast<float>(start.virt_height());

	// Default camera = canvas centre, identity zoom, zero rotation.
	// Matches ml-regl-js/src/app.js: `camera = [virtWidth/2, virtHeight/2, 1.0, 0.0]`.
	// Group-scoped cameras override this on the way down via the walker.
	render_ctx_->camera = {
		render_ctx_->view_w * 0.5f,
		render_ctx_->view_h * 0.5f,
		1.0f,
		0.0f,
	};
	int pw = 0, ph = 0;
	SDL_GetWindowSizeInPixels(window_, &pw, &ph);
	render_ctx_->pixel_w = pw;
	render_ctx_->pixel_h = ph;
	glViewport(0, 0, pw, ph);

	// Provision the FBO pool. JS ml-regl seeds at `fbo_num` and
	// grows on demand up to a hard cap (1000) — see
	// `getFreePalette` in ml-regl-js/src/app.js. Our FboPool
	// mirrors that: this initial count is a seed, not a ceiling;
	// `acquire` grows the pool with a warning when the seed is
	// exhausted. We default to 5 (matching the OCaml builders'
	// default) when the field is unset/zero.
	{
		const int fbo_count =
			start.fbo_num() > 0 ?
				static_cast<int>(start.fbo_num()) :
				5;
		if (!fbos_->init(fbo_count, pw, ph)) {
			set_error("FboPool::init failed");
			shutdown();
			return false;
		}
	}

	// Reasonable defaults for 2D drawing (matches the JS backend).
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
			    GL_ONE_MINUS_SRC_ALPHA);

	// Register and compile declarative programs. If StartRegl carries an
	// explicit builtin_programs list, mirror the JS backend and treat every
	// listed name as a required builtin. Unknown names are startup errors, not
	// silent no-ops. The [palette] blit is internal (JS creates drawPalette
	// separately), so keep it available even when a subset is requested.
	bool programs_ok = true;
	if (start.has_builtin_programs()) {
		(void)register_builtin_decl_program(*decl_programs_, "palette");
		for (const auto &name : start.builtin_programs().values()) {
			if (!register_builtin_decl_program(*decl_programs_, name)) {
				DECLGL_LOG_ERROR(
					"unknown builtin program requested in "
					"StartRegl.builtin_programs: '{}'",
					name);
				programs_ok = false;
			}
		}
	} else {
		register_builtin_decl_programs(*decl_programs_);
	}
	programs_ok = decl_programs_->compile_all() && programs_ok;
	if (!programs_ok) {
		set_error("failed to register/compile builtin programs");
		shutdown();
		return false;
	}

	start_ticks_ = SDL_GetTicks();
	set_error("");
	return true;
}

void Engine::dispatch_backend_command(
	const mlregl::transport::backend::BackendCommand &cmd)
{
	using namespace mlregl::transport::backend;
	switch (cmd.kind_case()) {
	case BackendCommand::kLoadTexture: {
		const auto &lt = cmd.load_texture();

		// Resolve filter / crop options (proto defaults if absent).
		using ProtoMin = mlregl::transport::backend::TextureMinOption;
		using ProtoMag = mlregl::transport::backend::TextureMagOption;
		ProtoMin min_opt = ProtoMin::TEXTURE_MIN_OPTION_LINEAR;
		ProtoMag mag_opt = ProtoMag::TEXTURE_MAG_OPTION_LINEAR;
		ImageCrop crop{};
		// Premultiply alpha at upload by default — see
		// [Texture::upload_rgba8] for the full rationale. The
		// proto field is `no_premultiply_alpha` (negated) so that
		// unset / proto3-default callers get the new sane
		// behaviour automatically.
		bool premultiply = true;
		if (lt.has_options()) {
			const auto &o = lt.options();
			min_opt = o.min();
			mag_opt = o.mag();
			if (o.has_crop()) {
				const auto &c = o.crop();
				crop.x = c.x();
				crop.y = c.y();
				crop.width = c.width();
				crop.height = c.height();
			}
			premultiply = !o.no_premultiply_alpha();
		}

		{
			DECLGL_LOG_INFO(
				"load_texture name={} url={} mag={} min={}, crop=({},{},{}x{}, premultiply={})",
				lt.name(), lt.url(), describe_mag(mag_opt),
				describe_min(min_opt), crop.x, crop.y,
				crop.width, crop.height, premultiply);
		}

		// Hand the decode off to the worker thread. The GL-side
		// upload + register + event-ship happens inside
		// [drain_ready_assets] at the top of the next [render()].
		//
		// Note: pre-StartRegl LoadTexture is fine here — the
		// worker doesn't touch GL. Decoded buffers sit in the
		// ready queue until [render()] is first called, at which
		// point we drain them and upload.
		if (!loader_) {
			// [init_decoders_only] should have been called already,
			// but be defensive.
			loader_ = std::make_unique<AssetLoader>();
		}
		DecodeJob job;
		job.kind = AssetKind::Texture;
		job.name = lt.name();
		job.image_url = lt.url();
		job.crop = crop;
		job.premultiply_alpha = premultiply;
		job.min_filter_enum = static_cast<int>(min_opt);
		job.mag_filter_enum = static_cast<int>(mag_opt);
		loader_->enqueue(std::move(job));
		break;
	}
	case BackendCommand::kLoadFont: {
		const auto &lf = cmd.load_font();
		DECLGL_LOG_INFO("load_font name={} image={} json={}", lf.name(),
				lf.image_url(), lf.json_url());

		// Off-thread: read+parse JSON, decode atlas PNG. The GL-
		// side glTexImage2D + Font/Texture registration + event
		// ship happens in [drain_ready_assets] on the GL thread.
		//
		// SDF atlas data MUST NOT be premultiplied — the worker
		// also asserts this defensively, but we set it explicitly
		// for clarity at the call site.
		if (!loader_) {
			loader_ = std::make_unique<AssetLoader>();
		}
		DecodeJob job;
		job.kind = AssetKind::Font;
		job.name = lf.name();
		job.image_url = lf.image_url();
		job.json_url = lf.json_url();
		job.premultiply_alpha = false;
		loader_->enqueue(std::move(job));
		break;
	}
	case BackendCommand::kConfigRegl:
		// Pacing + window flags live in the bridge — it owns the SDL
		// window and the per-frame loop, both of which ConfigRegl
		// targets. The bridge consumes kConfigRegl directly and never
		// forwards it here.
		break;
	case BackendCommand::kCreateProgram: {
		const auto &cp = cmd.create_program();
		DECLGL_LOG_INFO("create_program name={}", cp.name());
		BackendEvent ev;
		if (decl_programs_ && cp.has_program()) {
			auto prog = std::make_unique<programs::DynamicProgram>(
				cp.name(), cp.program());
			if (prog->compile()) {
				decl_programs_->register_program(
					std::move(prog));
				ev.mutable_program_created()->set_name(
					cp.name());
				ship_event(ev);
				break;
			}
		}
		ev.mutable_program_createfail()->set_name(cp.name());
		ship_event(ev);
		break;
	}
	case BackendCommand::kLoadAudio: {
		const auto &la = cmd.load_audio();
		DECLGL_LOG_INFO("load_audio url={}", la.audio_url());

		if (!audio_) {
			audio_ = std::make_unique<AudioEngine>();
		}
		// Open the audio device eagerly on the first LoadAudio so
		// we know the device sample rate to resample to in the
		// worker. JS does the equivalent inside its first
		// [decodeAudioData] call. Failure here ships an
		// audio_load_failed below; we still emit the load-failed
		// event so the OCaml side doesn't hang waiting on the
		// success/fail pair.
		if (!audio_->ensure_open()) {
			audio_->emit_load_failed(la.audio_url(),
						 AudioDecodeError::IoFailure);
			break;
		}

		if (!loader_) {
			loader_ = std::make_unique<AssetLoader>();
		}
		DecodeJob job;
		job.kind = AssetKind::Audio;
		job.name = la.audio_url();
		job.image_url = la.audio_url();
		job.audio_sample_rate = audio_->device_sample_rate();
		loader_->enqueue(std::move(job));
		break;
	}
	case BackendCommand::kUnloadTexture: {
		// Symmetric inverse of LoadTexture. Frees VRAM first
		// (TextureRegistry's unique_ptr destroys the [Texture],
		// whose dtor calls glDeleteTextures), then drops the slot
		// (the std::unordered_map::erase that happens inside
		// [unregister_texture] frees the CPU-side metadata).
		//
		// Three race cases worth thinking about:
		//   1. Texture was never loaded     → erase is a no-op.
		//   2. Load is queued but not yet
		//      decoded                      → cancel_pending drops it.
		//   3. Load is decoded but not yet
		//      drained onto the GL thread   → cancel_pending also
		//                                     drops it from
		//                                     ready_queue_.
		// Result: after this returns, no [texture_loaded] event
		// will ever ship for [name]. The OCaml side observes
		// exactly the unload it asked for.
		const auto &ut = cmd.unload_texture();
		DECLGL_LOG_INFO("unload_texture name={}", ut.name());
		if (loader_) {
			loader_->cancel_pending(AssetKind::Texture, ut.name());
		}
		if (textures_) {
			textures_->unregister_texture(ut.name());
		}
		break;
	}
	case BackendCommand::kUnloadFont: {
		// Symmetric inverse of LoadFont. Two GL resources to free:
		//   - the parsed [Font] in FontRegistry (CPU only)
		//   - the MSDF atlas [Texture] registered under the font's
		//     [image_url] in TextureRegistry (VRAM)
		// We resolve the atlas key from the font entry *before*
		// we erase the font, then unregister the texture.
		//
		// Note: a future enhancement could add atlas-sharing
		// refcounts (multiple fonts under different names but the
		// same image_url). Right now we eagerly free the atlas
		// even if another font still references it; in practice
		// the OCaml app loads each atlas exactly once, but if you
		// start sharing atlases this becomes a footgun. Mark this
		// for follow-up if/when atlas sharing actually shows up.
		const auto &uf = cmd.unload_font();
		DECLGL_LOG_INFO("unload_font name={}", uf.name());
		if (loader_) {
			loader_->cancel_pending(AssetKind::Font, uf.name());
		}
		std::string atlas_key;
		if (fonts_) {
			if (const auto *entry = fonts_->get(uf.name())) {
				atlas_key = entry->texture_name;
			}
			fonts_->unregister_font(uf.name());
		}
		if (textures_ && !atlas_key.empty()) {
			textures_->unregister_texture(atlas_key);
		}
		break;
	}
	case BackendCommand::kUnloadAudio: {
		const auto &ua = cmd.unload_audio();
		DECLGL_LOG_INFO("unload_audio url={}", ua.audio_url());
		if (loader_) {
			loader_->cancel_pending(AssetKind::Audio,
						ua.audio_url());
		}
		if (audio_) {
			audio_->unregister_buffer(ua.audio_url());
		}
		break;
	}
	case BackendCommand::kStartRegl:
		// The bridge handles StartRegl itself (it owns window+loop
		// lifecycle); it never forwards it here.
		break;
	case BackendCommand::kQuitRegl:
		// Same as StartRegl: handled by the bridge, never forwarded.
		break;
	case BackendCommand::KIND_NOT_SET:
	default:
		DECLGL_LOG_ERROR("unset command");
		break;
	}
}

bool Engine::exec_audio_cmd(const uint8_t *bytes, size_t len, double now_ms)
{
	if (!audio_) {
		audio_ = std::make_unique<AudioEngine>();
	}
	return audio_->exec_audio_cmd(bytes, len, now_ms);
}

void Engine::shutdown()
{
	// Stop the worker thread before tearing anything down. Pending
	// ready-but-undrained assets are dropped on the floor here — by
	// construction, OCaml hasn't received a *_loaded event for them
	// yet, so it can't be holding any references that need cleanup.
	if (loader_) {
		loader_->stop();
		loader_.reset();
	}
	// Tear the audio device down before SDL_Quit. AudioEngine's
	// destructor stops the SDL audio thread (SDL_DestroyAudioStream)
	// before any voice buffer is freed, so there's no risk of the
	// callback firing into freed memory.
	audio_.reset();
	walker_.reset();
	decl_programs_.reset();
	textures_.reset();
	fonts_.reset();
	fbos_.reset();
	render_ctx_.reset();
	if (gl_ctx_) {
		SDL_GL_DestroyContext(gl_ctx_);
		gl_ctx_ = nullptr;
	}
	if (window_) {
		SDL_DestroyWindow(window_);
		window_ = nullptr;
	}
	SDL_Quit();

	// Drain log records last so any teardown messages are visible.
	declgl::log::shutdown();
}

void Engine::ship_event(const mlregl::transport::backend::BackendEvent &ev)
{
	if (!event_sink_) {
		DECLGL_LOG_WARN(
			"ship_event: no EventSink registered "
			"(events will be dropped); is the bridge wired up?");
		return;
	}
	std::string buf;
	if (!ev.SerializeToString(&buf)) {
		DECLGL_LOG_ERROR(
			"ship_event: BackendEvent::SerializeToString failed");
		return;
	}
	event_sink_(reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
}

void Engine::drain_ready_assets(std::size_t max_items)
{
	if (!loader_ || !textures_ || !fonts_) {
		// Pre-StartRegl: GL not up yet, so we can't upload. Leave the
		// ready queue alone; we'll come back next [render()] once
		// [init_window_and_gl] has constructed the registries.
		return;
	}

	using namespace mlregl::transport::backend;

	std::vector<ReadyAsset> ready;
	ready.reserve(max_items);
	loader_->drain_ready(ready, max_items);

	for (auto &r : ready) {
		// ---- Audio ----
		// Doesn't touch GL: just register the PCM into the
		// AudioEngine (or ship audio_load_failed). Done first so
		// the texture/font failure-path doesn't accidentally grab
		// audio jobs on a path that calls *_loadfail.
		if (r.kind == AssetKind::Audio) {
			if (!audio_) {
				audio_ = std::make_unique<AudioEngine>();
			}
			if (r.audio_error != AudioDecodeError::None ||
			    !r.audio.ok()) {
				DECLGL_LOG_ERROR(
					"audio load '{}' failed: {}", r.name,
					r.error.empty() ? "decode" : r.error);
				audio_->emit_load_failed(
					r.image_url,
					r.audio_error !=
							AudioDecodeError::None ?
						r.audio_error :
						AudioDecodeError::DecodeFailure);
				continue;
			}
			audio_->register_buffer(r.image_url,
						std::move(r.audio));
			continue;
		}

		// ---- failure path (shared between texture / font) ----
		if (!r.error.empty() || !r.image.ok()) {
			DECLGL_LOG_ERROR("async load '{}' failed: {}", r.name,
					 r.error.empty() ? "decode/parse" :
							   r.error);
			BackendEvent ev;
			if (r.kind == AssetKind::Texture) {
				ev.mutable_texture_loadfail()->set_name(r.name);
			} else {
				ev.mutable_font_loadfail()->set_name(r.name);
			}
			ship_event(ev);
			continue;
		}

		if (r.kind == AssetKind::Texture) {
			using ProtoMin =
				mlregl::transport::backend::TextureMinOption;
			using ProtoMag =
				mlregl::transport::backend::TextureMagOption;
			const ProtoMin min_opt =
				static_cast<ProtoMin>(r.min_filter_enum);
			const ProtoMag mag_opt =
				static_cast<ProtoMag>(r.mag_filter_enum);
			const bool gen_mipmaps =
				min_filter_uses_mipmaps(min_opt);

			// Worker has already premultiplied (if requested). Pass
			// [premultiply_alpha=false] to upload_rgba8 to avoid a
			// second pass over the buffer.
			auto tex = std::make_unique<Texture>();
			if (!tex->upload_rgba8(r.image.width, r.image.height,
					       r.image.pixels.get(),
					       to_filter_min(min_opt),
					       to_filter_mag(mag_opt),
					       gen_mipmaps,
					       /*premultiply_alpha=*/false)) {
				DECLGL_LOG_ERROR(
					"async load '{}': upload failed",
					r.name);
				BackendEvent ev;
				ev.mutable_texture_loadfail()->set_name(r.name);
				ship_event(ev);
				continue;
			}

			const int tw = tex->width();
			const int th = tex->height();
			textures_->register_texture(r.name, std::move(tex));

			BackendEvent ev;
			auto *loaded = ev.mutable_texture_loaded();
			loaded->set_name(r.name);
			loaded->set_width(static_cast<uint32_t>(tw));
			loaded->set_height(static_cast<uint32_t>(th));
			ship_event(ev);
			continue;
		}

		// ---- Font ----
		// [r.font] is the parsed BMFont metric table; [r.image] is the
		// already-decoded MSDF atlas (NOT premultiplied, by design).
		// We register the atlas under [image_url] so multiple fonts
		// can reuse the same texture (mirrors JS behaviour).
		auto tex = std::make_unique<Texture>();
		if (!tex->upload_rgba8(
			    r.image.width, r.image.height, r.image.pixels.get(),
			    TextureFilter::Linear, TextureFilter::Linear,
			    /*generate_mipmaps=*/false,
			    /*premultiply_alpha=*/false)) {
			DECLGL_LOG_ERROR(
				"async load '{}': font atlas upload failed",
				r.name);
			BackendEvent ev;
			ev.mutable_font_loadfail()->set_name(r.name);
			ship_event(ev);
			continue;
		}
		textures_->register_texture(r.image_url, std::move(tex));

		DECLGL_LOG_INFO(
			"'{}': {} glyphs, {} kernings, "
			"{}x{} atlas, lineHeight={} base={} range={:.1f}",
			r.name, r.font->glyph_count(), r.font->kerning_count(),
			r.font->scaleW(), r.font->scaleH(),
			r.font->lineHeight(), r.font->base(),
			r.font->distanceRange());
		fonts_->register_font(r.name, std::move(r.font), r.image_url);

		BackendEvent ev;
		ev.mutable_font_loaded()->set_name(r.name);
		ship_event(ev);
	}
}

void Engine::render(const mlregl::transport::render::Renderable &tree)
{
	if (!walker_ || !render_ctx_)
		return;

	// Pop and finish up to N decoded assets per frame. The cap keeps a
	// burst of completed loads from spiking frame time. 4 is generous
	// for typical workloads (a single PNG upload is ~µs to a couple of
	// ms even for a few-MB texture); raise if you ever see assets
	// backing up in the queue.
	drain_ready_assets(/*max_items=*/4);

	// Update pixel viewport in case the window was resized. Resize the
	// FBO pool to match — we always keep palettes the size of the
	// drawing buffer so [palette] / [defaultCompositor] / [compFade]
	// sample 1:1 when blitting to the system framebuffer.
	if (window_) {
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(window_, &pw, &ph);
		if (pw != render_ctx_->pixel_w || ph != render_ctx_->pixel_h) {
			render_ctx_->pixel_w = pw;
			render_ctx_->pixel_h = ph;
			glViewport(0, 0, pw, ph);
			if (fbos_)
				fbos_->resize_all(pw, ph);
		}
	}

	// Mark every palette free for this frame. Acquire/release happens
	// inside the walker as it descends through groups & composites.
	if (fbos_)
		fbos_->free_all();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	walker_->render(tree, *render_ctx_);
}

} // namespace declgl
