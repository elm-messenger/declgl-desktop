// declgl.h — Public C ABI for libdeclgl.
//
// This header defines the only surface OCaml (or any other language with C
// FFI) needs to drive the desktop backend. All types are plain C; the
// implementation behind them is C++.
//
// Lifecycle:
//   1. declgl_init(...) -> declgl_engine_t*
//   2. declgl_set_callbacks(eng, &cb)
//   3. (optional) declgl_exec_backend_cmd(eng, ...) for one-time setup
//   4. while (declgl_should_run(eng)) declgl_run_frame(eng);
//   5. declgl_shutdown(eng)
//
// Threading:
//   All declgl_* functions must be called from the same thread that called
//   declgl_init(). All callbacks fire on that same thread, before
//   declgl_run_frame() returns. Internal worker threads (audio device, IO
//   pool) never call into user code directly.

#ifndef DECLGL_H_
#define DECLGL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(DECLGL_BUILDING_DLL)
#    define DECLGL_API __declspec(dllexport)
#  elif defined(DECLGL_USING_DLL)
#    define DECLGL_API __declspec(dllimport)
#  else
#    define DECLGL_API
#  endif
#else
#  define DECLGL_API __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Opaque handle to an engine instance. Created by declgl_init and freed by
// declgl_shutdown.
typedef struct declgl_engine declgl_engine_t;

// Status codes returned by ABI functions. 0 == OK, anything else is an
// error. Currently the engine is fairly forgiving and uses these only for
// hard failures (init failed, decode failed, ...).
typedef enum declgl_status {
    DECLGL_OK                = 0,
    DECLGL_ERR_GENERIC       = 1,
    DECLGL_ERR_INIT_FAILED   = 2,
    DECLGL_ERR_DECODE_FAILED = 3,
    DECLGL_ERR_INVALID_ARG   = 4
} declgl_status_t;

// Categorizes the protobuf payload coming back from the engine to the host
// (OCaml). The host decodes the bytes as the corresponding pb message.
typedef enum declgl_event_kind {
    DECLGL_EVENT_BACKEND = 1,  // mlregl.transport.backend.BackendEvent
    DECLGL_EVENT_AUDIO   = 2,  // mlregl.transport.audio.AudioBackendEvent
    DECLGL_EVENT_INPUT   = 3   // (M3+) input/window event, encoding TBD
} declgl_event_kind_t;

// Initial config for declgl_init. All strings are UTF-8 and must remain
// valid only for the duration of the call.
typedef struct declgl_init_config {
    const char* window_title;       // nullable -> default
    int32_t     window_width;       // <=0 -> default 1280
    int32_t     window_height;      // <=0 -> default 720
    const char* asset_root;         // nullable -> "" (cwd-relative)
    int32_t     io_thread_count;    // <=0 -> min(4, hardware_concurrency())
} declgl_init_config_t;

// Per-frame view callback: the host (OCaml) returns the encoded bytes of a
// `mlregl.transport.render.Renderable` describing what to draw this frame.
//
// The buffer pointed to by *out_bytes must remain valid until the next call
// to declgl_run_frame on this engine. A typical implementation owns a
// per-engine bytes buffer that it overwrites every frame.
//
// Returning *out_len == 0 means "render nothing this frame".
typedef declgl_status_t (*declgl_view_fn)(
    void*           userdata,
    const uint8_t** out_bytes,
    size_t*         out_len);

// Generic event callback: backend / audio / input events flow this way.
// `bytes` is owned by the engine and only valid for the duration of the
// call.
typedef void (*declgl_event_fn)(
    void*               userdata,
    declgl_event_kind_t kind,
    const uint8_t*      bytes,
    size_t              len);

// Bundle of host callbacks. Any field may be NULL, in which case the
// engine silently drops events of that kind.
typedef struct declgl_callbacks {
    void*           userdata;
    declgl_view_fn  view;
    declgl_event_fn event;
} declgl_callbacks_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize SDL3 + GL ctx + worker pool + audio device. Returns NULL on
// failure (call declgl_last_error() for a static string).
DECLGL_API declgl_engine_t* declgl_init(const declgl_init_config_t* cfg);

// Install host callbacks. The pointers are copied; the struct itself need
// not outlive the call.
DECLGL_API void declgl_set_callbacks(declgl_engine_t*           eng,
                                     const declgl_callbacks_t*  cb);

// True until the user closes the window or DECLGL_OK is broken irrecoverably.
DECLGL_API int32_t declgl_should_run(declgl_engine_t* eng);

// Drive one frame:
//   1. pump SDL events  -> emit DECLGL_EVENT_INPUT
//   2. drain IO/audio completions -> emit DECLGL_EVENT_BACKEND/AUDIO
//   3. ask host for a Renderable via cb.view
//   4. render + swap
DECLGL_API declgl_status_t declgl_run_frame(declgl_engine_t* eng);

// Tear down. Safe to call with NULL.
DECLGL_API void declgl_shutdown(declgl_engine_t* eng);

// Static, thread-local-ish error string for the last failure on this
// engine. Returns "" if no error.
DECLGL_API const char* declgl_last_error(void);

// ---------------------------------------------------------------------------
// Wire protocol entry points (host -> engine)
// ---------------------------------------------------------------------------

// Decode a `mlregl.transport.backend.BackendCommandBatch` protobuf payload
// and apply each command (load_texture, load_font, start_regl, ...).
// Asynchronous loads return their result later via the event callback.
DECLGL_API declgl_status_t declgl_exec_backend_cmd(declgl_engine_t* eng,
                                                   const uint8_t*   bytes,
                                                   size_t           len);

// Decode a `mlregl.transport.audio.AudioCommandBatch` and apply it to the
// audio engine.
DECLGL_API declgl_status_t declgl_exec_audio_cmd(declgl_engine_t* eng,
                                                 const uint8_t*   bytes,
                                                 size_t           len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DECLGL_H_
