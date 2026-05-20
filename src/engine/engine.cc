// engine.cc — Internal C++ engine implementation.

#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "transport_audio.pb.h"
#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl {

namespace {

// Static last-error string. We use a function-local static so it's lazily
// initialized and avoids any header-only state.
std::string& last_error_storage() {
    static std::string s;
    return s;
}

const char* describe_min(mlregl::transport::backend::TextureMinOption m) {
    using M = mlregl::transport::backend::TextureMinOption;
    switch (m) {
        case M::TEXTURE_MIN_OPTION_LINEAR:                  return "LINEAR";
        case M::TEXTURE_MIN_OPTION_NEAREST:                 return "NEAREST";
        case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST:  return "N_MIP_N";
        case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST:   return "L_MIP_N";
        case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR:   return "N_MIP_L";
        case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR:    return "L_MIP_L";
        default:                                            return "?";
    }
}

const char* describe_mag(mlregl::transport::backend::TextureMagOption m) {
    using M = mlregl::transport::backend::TextureMagOption;
    return m == M::TEXTURE_MAG_OPTION_NEAREST ? "NEAREST" : "LINEAR";
}

}  // namespace

void set_error(std::string msg) { last_error_storage() = std::move(msg); }

const char* last_error() { return last_error_storage().c_str(); }

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() = default;
Engine::~Engine() { shutdown(); }

bool Engine::init(const declgl_init_config_t& cfg) {
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

    const char* title = (cfg.window_title && *cfg.window_title)
                            ? cfg.window_title
                            : "declgl";
    const int32_t w = cfg.window_width  > 0 ? cfg.window_width  : 1280;
    const int32_t h = cfg.window_height > 0 ? cfg.window_height : 720;

    window_ = SDL_CreateWindow(
        title, w, h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) {
        set_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        SDL_Quit();
        return false;
    }

    gl_ctx_ = SDL_GL_CreateContext(window_);
    if (!gl_ctx_) {
        set_error(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
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

    std::printf("[declgl] GL %d.%d  vendor=%s  renderer=%s  glsl=%s\n",
                GLAD_VERSION_MAJOR(gl_version),
                GLAD_VERSION_MINOR(gl_version),
                reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    asset_root_ = (cfg.asset_root && *cfg.asset_root) ? cfg.asset_root : "";
    io_threads_ = cfg.io_thread_count > 0
                      ? cfg.io_thread_count
                      : std::min<int32_t>(
                            4,
                            static_cast<int32_t>(std::max(1u,
                                std::thread::hardware_concurrency())));
    start_ticks_ = SDL_GetTicks();
    running_     = true;
    set_error("");
    return true;
}

void Engine::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) running_ = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                glViewport(0, 0, ev.window.data1, ev.window.data2);
                break;
            default:
                break;
        }
        // M3+: forward selected events to host as DECLGL_EVENT_INPUT.
    }
}

void Engine::render_view() {
    // Placeholder render: animated clear color so we can see the loop is
    // alive. Real renderer arrives in M3.
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    glViewport(0, 0, w, h);

    const float t  = static_cast<float>(SDL_GetTicks() - start_ticks_) * 0.001f;
    const float r  = 0.5f + 0.5f * std::sin(t * 0.7f);
    const float g  = 0.5f + 0.5f * std::sin(t * 0.9f + 2.0f);
    const float b  = 0.5f + 0.5f * std::sin(t * 1.1f + 4.0f);

    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (callbacks_.view) {
        const uint8_t* bytes = nullptr;
        size_t         len   = 0;
        if (callbacks_.view(callbacks_.userdata, &bytes, &len) == DECLGL_OK
            && bytes && len > 0) {
            // M2: decode and report. M3+: actually render.
            mlregl::transport::render::Renderable r;
            if (r.ParseFromArray(bytes, static_cast<int>(len))) {
                std::printf("[declgl] view: Renderable kind=%d\n",
                            static_cast<int>(r.kind_case()));
            } else {
                std::fprintf(stderr,
                             "[declgl] view: failed to parse Renderable (%zu B)\n",
                             len);
            }
        }
    }

    SDL_GL_SwapWindow(window_);
}

declgl_status_t Engine::run_frame() {
    pump_events();
    render_view();
    return DECLGL_OK;
}

declgl_status_t Engine::exec_backend_cmd(const uint8_t* bytes, size_t len) {
    using namespace mlregl::transport::backend;
    BackendCommandBatch batch;
    if (!batch.ParseFromArray(bytes, static_cast<int>(len))) {
        set_error("exec_backend_cmd: ParseFromArray failed");
        return DECLGL_ERR_DECODE_FAILED;
    }

    std::printf("[declgl] backend: BackendCommandBatch with %d commands\n",
                batch.commands_size());

    for (const auto& cmd : batch.commands()) {
        switch (cmd.kind_case()) {
            case BackendCommand::kLoadTexture: {
                const auto& lt = cmd.load_texture();
                std::printf("  - load_texture name=%s url=%s",
                            lt.name().c_str(), lt.url().c_str());
                if (lt.has_options()) {
                    const auto& o = lt.options();
                    std::printf(" mag=%s min=%s",
                                describe_mag(o.mag()),
                                describe_min(o.min()));
                    if (o.has_crop()) {
                        const auto& c = o.crop();
                        std::printf(" crop=(%d,%d,%dx%d)",
                                    c.x(), c.y(), c.width(), c.height());
                    }
                }
                std::printf("\n");
                break;
            }
            case BackendCommand::kLoadFont: {
                const auto& lf = cmd.load_font();
                std::printf("  - load_font name=%s image=%s json=%s\n",
                            lf.name().c_str(),
                            lf.image_url().c_str(),
                            lf.json_url().c_str());
                break;
            }
            case BackendCommand::kConfigRegl: {
                std::printf("  - config_regl interval_ms=%g\n",
                            cmd.config_regl().interval_ms());
                break;
            }
            case BackendCommand::kStartRegl: {
                const auto& sr = cmd.start_regl();
                std::printf("  - start_regl virt=%gx%g fbo_num=%u",
                            sr.virt_width(), sr.virt_height(),
                            sr.fbo_num());
                if (sr.has_builtin_programs()) {
                    std::printf(" builtins=%d",
                                sr.builtin_programs().values_size());
                }
                std::printf("\n");
                break;
            }
            case BackendCommand::kCreateProgram: {
                const auto& cp = cmd.create_program();
                std::printf("  - create_program name=%s\n", cp.name().c_str());
                break;
            }
            case BackendCommand::kLoadAudio: {
                std::printf("  - load_audio url=%s\n",
                            cmd.load_audio().audio_url().c_str());
                break;
            }
            case BackendCommand::KIND_NOT_SET:
            default:
                std::printf("  - <unset command>\n");
                break;
        }
    }
    return DECLGL_OK;
}

declgl_status_t Engine::exec_audio_cmd(const uint8_t* bytes, size_t len) {
    using namespace mlregl::transport::audio;
    AudioCommandBatch batch;
    if (!batch.ParseFromArray(bytes, static_cast<int>(len))) {
        set_error("exec_audio_cmd: ParseFromArray failed");
        return DECLGL_ERR_DECODE_FAILED;
    }

    std::printf("[declgl] audio: AudioCommandBatch with %d actions\n",
                batch.actions_size());

    for (const auto& act : batch.actions()) {
        switch (act.kind_case()) {
            case AudioAction::kStartSound: {
                const auto& s = act.start_sound();
                std::printf("  - start_sound group=%u buf=%u t=%g start_at=%g vol=%g\n",
                            s.node_group_id(), s.buffer_id(),
                            s.start_time(), s.start_at(), s.volume());
                break;
            }
            case AudioAction::kStopSound:
                std::printf("  - stop_sound group=%u\n",
                            act.stop_sound().node_group_id());
                break;
            case AudioAction::kSetVolume:
                std::printf("  - set_volume group=%u vol=%g\n",
                            act.set_volume().node_group_id(),
                            act.set_volume().volume());
                break;
            case AudioAction::kSetVolumeAt:
                std::printf("  - set_volume_at group=%u (%d timelines)\n",
                            act.set_volume_at().node_group_id(),
                            act.set_volume_at().volume_at_size());
                break;
            case AudioAction::kSetLoopConfig:
                std::printf("  - set_loop_config group=%u\n",
                            act.set_loop_config().node_group_id());
                break;
            case AudioAction::kSetPlaybackRate:
                std::printf("  - set_playback_rate group=%u rate=%g\n",
                            act.set_playback_rate().node_group_id(),
                            act.set_playback_rate().playback_rate());
                break;
            case AudioAction::KIND_NOT_SET:
            default:
                std::printf("  - <unset action>\n");
                break;
        }
    }
    return DECLGL_OK;
}

void Engine::shutdown() {
    if (gl_ctx_) {
        SDL_GL_DestroyContext(gl_ctx_);
        gl_ctx_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    // Quit only if we'd called Init successfully. Calling SDL_Quit twice is
    // harmless but noisy.
    SDL_Quit();
    running_ = false;
}

}  // namespace declgl
