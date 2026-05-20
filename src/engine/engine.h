// engine.h — Internal C++ engine class (NOT part of the public ABI).
//
// declgl_engine_t (the opaque C handle) is just a typedef to this class.
// All the SDL3 / GL / decode work lives here. The c_api/ shim translates
// the C ABI to method calls on this object.

#pragma once

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <cstdint>
#include <string>

#include "c_api/declgl.h"

namespace declgl {

// Internal engine state. Lifetime is owned by the C ABI shim.
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // Bring up SDL3 + window + GL ctx + glad. Returns false on failure;
    // caller should check declgl::last_error() (set via set_error()).
    bool init(const declgl_init_config_t& cfg);

    void set_callbacks(const declgl_callbacks_t& cb) { callbacks_ = cb; }

    // True until the user closes the window.
    bool should_run() const { return running_; }

    // One frame: pump events, request view from host, draw, swap.
    declgl_status_t run_frame();

    // Decode + dispatch a BackendCommandBatch / AudioCommandBatch.
    declgl_status_t exec_backend_cmd(const uint8_t* bytes, size_t len);
    declgl_status_t exec_audio_cmd  (const uint8_t* bytes, size_t len);

    void shutdown();

private:
    void pump_events();
    void render_view();

    SDL_Window*          window_   = nullptr;
    SDL_GLContext        gl_ctx_   = nullptr;
    bool                 running_  = false;

    declgl_callbacks_t   callbacks_{};
    std::string          asset_root_;
    int32_t              io_threads_ = 0;
    Uint64               start_ticks_ = 0;
};

// Set / read a static last-error string. Set by failing calls; cleared by
// successful ones via set_error("").
void        set_error(std::string msg);
const char* last_error();

}  // namespace declgl
