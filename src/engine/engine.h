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
#include <memory>
#include <string>

#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl {

class ProgramRegistry;
class RenderableWalker;
struct RenderContext;

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // Phase 1: cheap constructor-side setup. Currently a no-op — kept as
    // an explicit step so future decoder/cache state has a clear home.
    void init_decoders_only();

    // Phase 2: bring up SDL3 + window + GL ctx + glad. Driven by a
    // [StartRegl] BackendCommand. Returns false on failure; caller can
    // read [last_error()].
    bool init_window_and_gl(
        const mlregl::transport::backend::StartRegl& start);

    // Per-command dispatch (decode-and-log for now; M3+ wires real work).
    // The bridge calls this for every non-StartRegl BackendCommand.
    void dispatch_backend_command(
        const mlregl::transport::backend::BackendCommand& cmd);

    // Decode + dispatch an AudioCommandBatch. Returns false on parse
    // failure.
    bool exec_audio_cmd(const uint8_t* bytes, size_t len);

    // Walk a Renderable tree and emit the corresponding GL draw calls
    // onto the currently-bound framebuffer. Per-frame entry point for
    // the bridge.
    void render(const mlregl::transport::render::Renderable& tree);

    void shutdown();

    // Accessor used by the caml_bridge for SwapWindow / pixel-size.
    SDL_Window* sdl_window() const { return window_; }

private:
    SDL_Window*    window_      = nullptr;
    SDL_GLContext  gl_ctx_      = nullptr;
    std::string    asset_root_;
    Uint64         start_ticks_ = 0;

    // M3.B+: GPU resources. Lazily constructed in init_window_and_gl
    // because they require an active GL context.
    std::unique_ptr<ProgramRegistry>  programs_;
    std::unique_ptr<RenderableWalker> walker_;
    std::unique_ptr<RenderContext>    render_ctx_;
};

// Static last-error string. Set by failing engine calls; cleared by
// successful ones via set_error("").
void        set_error(std::string msg);
const char* last_error();

}  // namespace declgl
