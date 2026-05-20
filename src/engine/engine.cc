// engine.cc — Internal C++ engine implementation for the desktop backend.

#include "engine/engine.h"

#include <algorithm>
#include <cstdio>

#include "transport_audio.pb.h"
#include "transport_backend.pb.h"

namespace declgl {

namespace {

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

void Engine::init_decoders_only() {
    // No persistent decoder state yet. Reserved for M3+ caches.
    set_error("");
}

bool Engine::init_window_and_gl(
    const mlregl::transport::backend::StartRegl& start) {

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
    const int32_t w = start.virt_width()  > 0
                          ? static_cast<int32_t>(start.virt_width())
                          : 1280;
    const int32_t h = start.virt_height() > 0
                          ? static_cast<int32_t>(start.virt_height())
                          : 720;

    window_ = SDL_CreateWindow(
        "declgl", w, h,
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

    if (start.has_builtin_programs()) {
        std::printf("[declgl] start: virt=%gx%g fbo_num=%u builtins=%d\n",
                    start.virt_width(), start.virt_height(),
                    start.fbo_num(),
                    start.builtin_programs().values_size());
    } else {
        std::printf("[declgl] start: virt=%gx%g fbo_num=%u\n",
                    start.virt_width(), start.virt_height(),
                    start.fbo_num());
    }

    start_ticks_ = SDL_GetTicks();
    set_error("");
    return true;
}

void Engine::dispatch_backend_command(
    const mlregl::transport::backend::BackendCommand& cmd) {
    using namespace mlregl::transport::backend;
    switch (cmd.kind_case()) {
        case BackendCommand::kLoadTexture: {
            const auto& lt = cmd.load_texture();
            std::printf("[declgl] load_texture name=%s url=%s",
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
            std::printf("[declgl] load_font name=%s image=%s json=%s\n",
                        lf.name().c_str(),
                        lf.image_url().c_str(),
                        lf.json_url().c_str());
            break;
        }
        case BackendCommand::kConfigRegl: {
            std::printf("[declgl] config_regl interval_ms=%g\n",
                        cmd.config_regl().interval_ms());
            break;
        }
        case BackendCommand::kCreateProgram: {
            const auto& cp = cmd.create_program();
            std::printf("[declgl] create_program name=%s\n", cp.name().c_str());
            break;
        }
        case BackendCommand::kLoadAudio: {
            std::printf("[declgl] load_audio url=%s\n",
                        cmd.load_audio().audio_url().c_str());
            break;
        }
        case BackendCommand::kStartRegl:
            // The bridge handles StartRegl itself (it owns window+loop
            // lifecycle); it never forwards it here.
            break;
        case BackendCommand::KIND_NOT_SET:
        default:
            std::fprintf(stderr, "[declgl] <unset command>\n");
            break;
    }
}

bool Engine::exec_audio_cmd(const uint8_t* bytes, size_t len) {
    using namespace mlregl::transport::audio;
    AudioCommandBatch batch;
    if (!batch.ParseFromArray(bytes, static_cast<int>(len))) {
        set_error("exec_audio_cmd: ParseFromArray failed");
        return false;
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
                std::fprintf(stderr, "  - <unset action>\n");
                break;
        }
    }
    return true;
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
    SDL_Quit();
}

}  // namespace declgl
