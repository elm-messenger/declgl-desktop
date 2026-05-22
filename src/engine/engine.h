// engine.h — Internal C++ engine class for the desktop ml_regl backend.
//
// The OCaml caml_bridge is the only public entry point into this code.
// The bridge owns its own per-frame loop; the engine owns SDL/GL state +
// per-command decode/dispatch.
//
// Lifetime, mirroring the JS backend:
//
//   1. The bridge constructs an Engine the first time OCaml ships any
//      command, and immediately calls [init_decoders_only].
//   2. Subsequent BackendCommand kinds (LoadTexture, ConfigRegl,
//      CreateProgram, LoadFont, LoadAudio) are forwarded one by one via
//      [dispatch_backend_command].
//   3. When a [StartRegl] arrives, the bridge calls
//      [init_window_and_gl(start)] to bring up SDL3 + window + GL ctx +
//      glad, then enters its per-frame loop. The engine is otherwise
//      passive — events are pumped by the bridge directly from SDL.
//   4. On user-requested exit, the bridge calls [shutdown].

#pragma once

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl
{

class ProgramRegistry;
class DeclProgramRegistry;
class TextureRegistry;
class FontRegistry;
class RenderableWalker;
class FboPool;
class AssetLoader;
class AudioEngine;
struct RenderContext;

// Bridge → engine sink for serialized BackendEvent payloads. The
// engine encodes a [mlregl::transport::backend::BackendEvent]
// (containing one of TextureLoaded / TextureLoadFail / FontLoaded /
// ProgramCreated / ...), serializes it to bytes, and calls this. The
// bridge marshals those bytes to the OCaml
// `Callback.register "declgl_app_recv_regl_cmd_pb"` handler.
//
// This indirection keeps the engine OCaml-runtime-agnostic — useful
// for unit tests that exercise the engine without booting OCaml.
using EventSink = std::function<void(const uint8_t *bytes, std::size_t len)>;

class Engine {
    public:
	Engine();
	~Engine();

	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;

	// Phase 1: cheap constructor-side setup. Currently a no-op — kept as
	// an explicit step so future decoder/cache state has a clear home.
	void init_decoders_only();

	// Phase 2: bring up SDL3 + window + GL ctx + glad. Driven by a
	// [StartRegl] BackendCommand. Returns false on failure; caller can
	// read [last_error()].
	bool
	init_window_and_gl(const mlregl::transport::backend::StartRegl &start);

	// Per-command dispatch (decode-and-log for now; M3+ wires real work).
	// The bridge calls this for every non-StartRegl BackendCommand.
	void dispatch_backend_command(
		const mlregl::transport::backend::BackendCommand &cmd);

	// Decode + dispatch an AudioCommandBatch. Returns false on parse
	// failure. [now_ms] is OCaml's wall clock at the moment OCaml
	// shipped the batch — used as the time anchor for start_time /
	// volume timeline scheduling. Same clock convention as JS's
	// [Date.now()].
	bool exec_audio_cmd(const uint8_t *bytes, size_t len, double now_ms);

	// Walk a Renderable tree and emit the corresponding GL draw calls
	// onto the currently-bound framebuffer. Per-frame entry point for
	// the bridge.
	void render(const mlregl::transport::render::Renderable &tree);

	void shutdown();

	// Accessor used by the caml_bridge for SwapWindow / pixel-size.
	SDL_Window *sdl_window() const
	{
		return window_;
	}

	// Register the bridge's OCaml-callback dispatcher. Must be called
	// before any backend command that may fire an event (e.g.
	// LoadTexture). If unset, events are silently dropped with a warning.
	void set_event_sink(EventSink sink)
	{
		event_sink_ = std::move(sink);
	}

	// Same as [set_event_sink] but for AudioBackendEvents (shipped
	// to OCaml via the [declgl_app_recv_audio_msg_pb] callback).
	// AudioContextReady, AudioLoadSuccess, AudioLoadFailed all
	// flow through this sink.
	void set_audio_event_sink(EventSink sink);

    private:
	// Helper used by [dispatch_backend_command]: encode `ev` and forward
	// through [event_sink_]. No-op (with a one-shot warning) if no sink
	// was registered.
	void ship_event(const mlregl::transport::backend::BackendEvent &ev);

	// Pop ready-asset records off [loader_] and finish them on the GL
	// thread: glTexImage2D, register in TextureRegistry / FontRegistry,
	// ship the corresponding _loaded / _loadfail event. Bounded per
	// call to keep frame time stable when a flood of assets land at
	// once. Called at the top of [render()].
	void drain_ready_assets(std::size_t max_items);

	SDL_Window *window_ = nullptr;
	SDL_GLContext gl_ctx_ = nullptr;
	std::string asset_root_;
	Uint64 start_ticks_ = 0;
	EventSink event_sink_;

	// M3.B+: GPU resources. Lazily constructed in init_window_and_gl
	// because they require an active GL context.
	std::unique_ptr<ProgramRegistry> programs_;
	std::unique_ptr<DeclProgramRegistry> decl_programs_;
	std::unique_ptr<TextureRegistry> textures_;
	std::unique_ptr<FontRegistry> fonts_;
	std::unique_ptr<FboPool> fbos_;
	std::unique_ptr<RenderableWalker> walker_;
	std::unique_ptr<RenderContext> render_ctx_;

	// Async asset decode pipeline. Owns one worker thread; safe to
	// construct before the GL context exists since it never touches GL.
	// Lives across the whole Engine lifetime: pre-StartRegl LoadTexture
	// commands enqueue here too, and their decoded buffers wait in the
	// ready queue until [render()] starts draining them. Destroyed
	// before the GL context in [shutdown()] so the worker can't outlive
	// anything it might hand off to.
	std::unique_ptr<AssetLoader> loader_;

	// Audio runtime: SDL device + mixer + voice table. Constructed
	// lazily on first audio activity (matches JS's lazy
	// AudioContext creation). Destroyed in [shutdown].
	std::unique_ptr<AudioEngine> audio_;
};

// Static last-error string. Set by failing engine calls; cleared by
// successful ones via set_error("").
void set_error(std::string msg);
const char *last_error();

} // namespace declgl
