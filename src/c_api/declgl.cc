// declgl.cc — C ABI shim. Each function delegates to declgl::Engine.
//
// We treat declgl_engine_t as an opaque alias for declgl::Engine. Casts
// between them are valid because the public type is declared as a forward
// reference and the implementation type is what we actually allocate.

#include "c_api/declgl.h"

#include <new>

#include "engine/engine.h"

namespace {

inline declgl::Engine* to_cpp(declgl_engine_t* h) {
    return reinterpret_cast<declgl::Engine*>(h);
}
inline declgl_engine_t* to_c(declgl::Engine* e) {
    return reinterpret_cast<declgl_engine_t*>(e);
}

}  // namespace

extern "C" {

DECLGL_API declgl_engine_t* declgl_init(const declgl_init_config_t* cfg) {
    declgl_init_config_t default_cfg{};
    const declgl_init_config_t& use_cfg = cfg ? *cfg : default_cfg;

    auto* eng = new (std::nothrow) declgl::Engine();
    if (!eng) {
        declgl::set_error("declgl_init: out of memory");
        return nullptr;
    }
    if (!eng->init(use_cfg)) {
        delete eng;
        return nullptr;
    }
    return to_c(eng);
}

DECLGL_API void declgl_set_callbacks(declgl_engine_t*          eng,
                                     const declgl_callbacks_t* cb) {
    if (!eng || !cb) return;
    to_cpp(eng)->set_callbacks(*cb);
}

DECLGL_API int32_t declgl_should_run(declgl_engine_t* eng) {
    return (eng && to_cpp(eng)->should_run()) ? 1 : 0;
}

DECLGL_API declgl_status_t declgl_run_frame(declgl_engine_t* eng) {
    if (!eng) return DECLGL_ERR_INVALID_ARG;
    return to_cpp(eng)->run_frame();
}

DECLGL_API void declgl_shutdown(declgl_engine_t* eng) {
    if (!eng) return;
    to_cpp(eng)->shutdown();
    delete to_cpp(eng);
}

DECLGL_API const char* declgl_last_error(void) {
    return declgl::last_error();
}

DECLGL_API declgl_status_t declgl_exec_backend_cmd(declgl_engine_t* eng,
                                                   const uint8_t*   bytes,
                                                   size_t           len) {
    if (!eng || !bytes || len == 0) return DECLGL_ERR_INVALID_ARG;
    return to_cpp(eng)->exec_backend_cmd(bytes, len);
}

DECLGL_API declgl_status_t declgl_exec_audio_cmd(declgl_engine_t* eng,
                                                 const uint8_t*   bytes,
                                                 size_t           len) {
    if (!eng || !bytes || len == 0) return DECLGL_ERR_INVALID_ARG;
    return to_cpp(eng)->exec_audio_cmd(bytes, len);
}

}  // extern "C"
