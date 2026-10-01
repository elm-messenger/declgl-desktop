// engine.h — Internal C++ engine class for the desktop ml_regl backend.
//
// [Runtime] (runtime/runtime.h) is the only caller of this code. The
// runtime owns the per-frame loop, SDL event pump and frame pacing; the
// engine owns SDL/GL state, resource registries, the asset loader, the
// audio engine and per-command decode/dispatch.
//
// Lifetime, mirroring the JS backend:
//
//   1. The runtime constructs an Engine the first time the host ships
//      any command, and immediately calls [init_decoders_only].
//   2. Every BackendCommand except StartRegl, QuitRegl and ConfigRegl
//      (which the runtime handles itself) is forwarded one by one via
//      [dispatch_backend_command].
//   3. When a [StartRegl] arrives, the runtime calls
//      [init_window_and_gl(start)] to bring up SDL3 + window + GL ctx +
//      glad, then enters its per-frame loop. The engine is otherwise
//      passive — the runtime pumps events directly from SDL and calls
//      [render] / [process_ready_assets] once per frame.
//   4. When the loop exits, the runtime calls [shutdown].

#pragma once

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl
{

class DeclProgramRegistry;
class TextureRegistry;
class FontRegistry;
class RenderableWalker;
class FboPool;
class AssetLoader;
class AudioEngine;
struct RenderContext;

// Engine → host sink for serialized BackendEvent payloads. The
// engine encodes a [mlregl::transport::backend::BackendEvent]
// (containing one of TextureLoaded / TextureLoadFail / FontLoaded /
// ProgramCreated / ...), serializes it to bytes, and calls this. The
// runtime forwards those bytes to [LoopHooks::on_backend_event]; the
// OCaml bridge then marshals them to the
// `Callback.register "declgl_app_recv_regl_cmd_pb"` handler.
//
// This indirection keeps the engine OCaml-runtime-agnostic — useful
// for unit tests that exercise the engine without booting OCaml.
using EventSink = std::function<void(const uint8_t *bytes, std::size_t len)>;

class Engine {
    public:
	explicit Engine(std::filesystem::path asset_root = {});
	~Engine();

	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;

	// Phase 1: GL-free setup. Starts the async log backend, the asset
	// loader worker and the (still closed) audio engine so loads shipped
	// before StartRegl can begin decoding immediately. Idempotent.
	void init_decoders_only();

	// Phase 2: bring up SDL3 + window + GL ctx + glad. Driven by a
	// [StartRegl] BackendCommand. Returns false on failure after logging
	// the concrete error.
	bool
	init_window_and_gl(const mlregl::transport::backend::StartRegl &start);

	// Per-command dispatch. The runtime calls this for every
	// BackendCommand except StartRegl, QuitRegl and ConfigRegl.
	void dispatch_backend_command(
		const mlregl::transport::backend::BackendCommand &cmd);

	// Decode + dispatch an AudioCommandBatch. Returns false on parse
	// failure. [now_ms] is the runtime clock (ms since the run loop
	// started, the same clock as UpdateTick.ts) at the moment the batch
	// was shipped — used as the time anchor for start_time / volume
	// timeline scheduling.
	bool exec_audio_cmd(const uint8_t *bytes, size_t len, double now_ms);

	// Walk a Renderable tree and emit the corresponding GL draw calls
	// onto the currently-bound framebuffer. Per-frame entry point for
	// the runtime. [max_assets_per_frame] bounds ready asset jobs
	// drained before drawing; 0 means unlimited.
	void render(const mlregl::transport::render::Renderable &tree,
		    std::size_t max_assets_per_frame);

	// Finish ready asynchronous asset jobs on a frame with no render tree.
	void process_ready_assets(std::size_t max_assets_per_frame);

	void shutdown();

	// Accessor used by the runtime for SwapWindow, window config, mouse
	// scaling and frame capture.
	SDL_Window *sdl_window() const
	{
		return window_;
	}

	// Register the host event dispatcher (installed by the runtime).
	// Must be called before any backend command that may fire an event
	// (e.g. LoadTexture). If unset, events are dropped with a warning.
	void set_event_sink(EventSink sink)
	{
		event_sink_ = std::move(sink);
	}

	// Same as [set_event_sink] but for AudioBackendEvents (shipped
	// to the host via [LoopHooks::on_audio_event]; the OCaml bridge uses
	// the [declgl_app_recv_audio_msg_pb] callback).
	// AudioContextReady, AudioLoadSuccess, AudioLoadFailed all
	// flow through this sink.
	void set_audio_event_sink(EventSink sink);

    private:
	// Helper used by [dispatch_backend_command]: encode `ev` and forward
	// through [event_sink_]. No-op (with a one-shot warning) if no sink
	// was registered.
	void ship_event(const mlregl::transport::backend::BackendEvent &ev);
	void ensure_kv_load_started();
	void enqueue_kv_persist();
	std::string kv_store_path() const;
	void ship_value_read_result(const std::string &key);

	// Pop ready-asset records off [loader_] and finish them on the GL
	// thread: glTexImage2D, register in TextureRegistry / FontRegistry,
	// ship the corresponding _loaded / _loadfail event. Bounded per
	// call to keep frame time stable when a flood of assets land at
	// once. Called at the top of [render()] and by
	// [process_ready_assets] on frames without a render tree.
	void drain_ready_assets(std::size_t max_items);

	SDL_Window *window_ = nullptr;
	SDL_GLContext gl_ctx_ = nullptr;
	bool sdl_initialized_ = false;
	// App identifier from StartRegl.app_name. Used to scope
	// SDL_GetPrefPath for KV storage so different apps don't
	// collide on the same per-user data directory. Empty until
	// StartRegl arrives; treated as "declgl" by [kv_store_path].
	std::string app_name_;
	Uint64 start_ticks_ = 0;
	EventSink event_sink_;
	bool kv_load_started_ = false;
	bool kv_loaded_ = false;
	std::unordered_map<std::string, std::string> kv_store_;
	std::unordered_set<std::string> kv_dirty_keys_;
	std::vector<std::string> pending_kv_reads_;

	// GPU resources. Lazily constructed in init_window_and_gl
	// because they require an active GL context.
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
	std::filesystem::path asset_root_;
};

} // namespace declgl
