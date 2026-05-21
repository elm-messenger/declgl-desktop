// engine.cc — Internal C++ engine implementation for the desktop backend.

#include "engine/engine.h"

#include <algorithm>
#include <cstdio>

#include "gpu/program_registry.h"
#include "gpu/fbo_pool.h"
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

// Map proto-level filter enums → engine-side [TextureFilter].
TextureFilter to_filter_min(mlregl::transport::backend::TextureMinOption m) {
    using M = mlregl::transport::backend::TextureMinOption;
    switch (m) {
        case M::TEXTURE_MIN_OPTION_NEAREST:                return TextureFilter::Nearest;
        case M::TEXTURE_MIN_OPTION_LINEAR:                 return TextureFilter::Linear;
        case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST: return TextureFilter::NearestMipmapNearest;
        case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST:  return TextureFilter::LinearMipmapNearest;
        case M::TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR:  return TextureFilter::NearestMipmapLinear;
        case M::TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR:   return TextureFilter::LinearMipmapLinear;
        default:                                           return TextureFilter::Linear;
    }
}

TextureFilter to_filter_mag(mlregl::transport::backend::TextureMagOption m) {
    using M = mlregl::transport::backend::TextureMagOption;
    return m == M::TEXTURE_MAG_OPTION_NEAREST ? TextureFilter::Nearest
                                              : TextureFilter::Linear;
}

// True iff [m] is one of the four mipmap minification modes.
bool min_filter_uses_mipmaps(mlregl::transport::backend::TextureMinOption m) {
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

}  // namespace

void set_error(std::string msg) { last_error_storage() = std::move(msg); }

const char* last_error() { return last_error_storage().c_str(); }

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() = default;
Engine::~Engine() { shutdown(); }

void Engine::init_decoders_only() {
    // The async asset decode pipeline doesn't touch GL, so we bring it
    // up here — well before [init_window_and_gl] — so any LoadTexture /
    // LoadFont commands that arrive between [init_decoders_only] and
    // [StartRegl] get decoded in parallel with the rest of startup.
    // Their ready buffers sit in the loader's queue until [render()]
    // starts draining them.
    if (!loader_) {
        loader_ = std::make_unique<AssetLoader>();
    }
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

    // M3.B: spin up the program registry, render context and walker.
    // These all need an active GL context, so we construct them here
    // rather than in [init_decoders_only].
    programs_   = std::make_unique<ProgramRegistry>();
    textures_   = std::make_unique<TextureRegistry>();
    fonts_      = std::make_unique<FontRegistry>();
    fbos_       = std::make_unique<FboPool>();
    render_ctx_ = std::make_unique<RenderContext>();
    walker_     = std::make_unique<RenderableWalker>(*programs_);
    render_ctx_->textures = textures_.get();
    render_ctx_->fonts    = fonts_.get();
    render_ctx_->fbos     = fbos_.get();

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

    // Provision the FBO pool. JS ml-regl mirrors StartRegl.fbo_num
    // exactly — running out of palettes mid-frame is a hard error
    // there. We default to 5 (matching the OCaml builders' default)
    // when the field is unset/zero.
    {
        const int fbo_count =
            start.fbo_num() > 0 ? static_cast<int>(start.fbo_num()) : 5;
        if (!fbos_->init(fbo_count, pw, ph)) {
            set_error("FboPool::init failed");
            shutdown();
            return false;
        }
    }

    // Reasonable defaults for 2D drawing (matches the JS backend).
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Register every builtin requested by StartRegl. Names that have no
    // vendored GLSL produce a warning but don't fail startup — that lets
    // us light up programs incrementally across milestones.
    if (start.has_builtin_programs()) {
        for (const auto& name : start.builtin_programs().values()) {
            programs_->register_builtin(name);
        }
    }

    // Mirror the JS backend, which makes every shader in its vendored
    // {frag,vert}.glsl tables unconditionally available regardless of
    // whether the user enumerated them in [builtin_programs]. As we
    // port more shaders we extend this list. Already-registered names
    // are short-circuited.
    //
    // Most builtins use their own (vert, frag) pair under the same
    // name, but two need cross-program pairing to match the JS
    // backend's behaviour:
    //   - `roundedRect` reuses circle's vert with its own frag
    //   - `quad` and `poly` reuse the entire `triangle` pair
    struct BuiltinSpec {
        const char* name;
        const char* vert_from;  // builtin name to pull vertex source
        const char* frag_from;  // builtin name to pull fragment source
    };
    static const BuiltinSpec kAlwaysOnBuiltins[] = {
        // M3.B: pure fill triangle
        { "triangle",    "triangle", "triangle" },
        // M3.C: more 2D primitives
        { "rect",        "rect",     "rect"     },
        { "circle",      "circle",   "circle"   },
        { "roundedRect", "circle",   "roundedRect" },
        { "quad",        "triangle", "triangle" },
        { "poly",        "triangle", "triangle" },
        // M3.D: textured primitives. `texture` and `textureCropped`
        // share a single (vert, frag) pair — the difference is whether
        // the walker sources the per-vertex `texc` from the call or
        // from a hardcoded fullscreen-UV table. Likewise
        // `centeredCroppedTexture` reuses the centered frag (both
        // read the `vuv` varying).
        { "texture",                "texture",                "texture"          },
        { "textureCropped",         "texture",                "texture"          },
        { "centeredTexture",        "textureCentered",        "textureCentered"  },
        { "centeredCroppedTexture", "textureCroppedCentered", "textureCentered"  },
        // M3.E: effect & compositor programs. Every one of these uses
        // the shared [effect.vert.glsl] which projects the unit-quad
        // corner directly to NDC and passes the texc through as `uv`.
        // The differences are entirely in the fragment shader.
        { "palette",            "effect", "palette"           },
        { "defaultCompositor",  "effect", "defaultCompositor" },
        { "compFade",           "effect", "compFade"          },
        { "alphamult",          "effect", "alphamult"         },
        { "colormult",          "effect", "colormult"         },
        // M3.F: MSDF text. Single program; the walker generates the
        // per-frame quad VBO from the [Font]'s glyph table on demand.
        { "textbox",            "text",   "text"              },
    };
    for (const auto& s : kAlwaysOnBuiltins) {
        if (!programs_->get(s.name)) {
            programs_->register_builtin_alias(s.name, s.vert_from, s.frag_from);
        }
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
                const auto& o = lt.options();
                min_opt = o.min();
                mag_opt = o.mag();
                if (o.has_crop()) {
                    const auto& c = o.crop();
                    crop.x      = c.x();
                    crop.y      = c.y();
                    crop.width  = c.width();
                    crop.height = c.height();
                }
                premultiply = !o.no_premultiply_alpha();
            }

            std::printf("[declgl] load_texture name=%s url=%s mag=%s min=%s",
                        lt.name().c_str(), lt.url().c_str(),
                        describe_mag(mag_opt), describe_min(min_opt));
            if (crop.width > 0 && crop.height > 0) {
                std::printf(" crop=(%d,%d,%dx%d)",
                            crop.x, crop.y, crop.width, crop.height);
            }
            std::printf("\n");

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
            job.kind              = AssetKind::Texture;
            job.name              = lt.name();
            job.image_url         = lt.url();
            job.crop              = crop;
            job.premultiply_alpha = premultiply;
            job.min_filter_enum   = static_cast<int>(min_opt);
            job.mag_filter_enum   = static_cast<int>(mag_opt);
            loader_->enqueue(std::move(job));
            break;
        }
        case BackendCommand::kLoadFont: {
            const auto& lf = cmd.load_font();
            std::printf("[declgl] load_font name=%s image=%s json=%s\n",
                        lf.name().c_str(),
                        lf.image_url().c_str(),
                        lf.json_url().c_str());

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
            job.kind              = AssetKind::Font;
            job.name              = lf.name();
            job.image_url         = lf.image_url();
            job.json_url          = lf.json_url();
            job.premultiply_alpha = false;
            loader_->enqueue(std::move(job));
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
            if (programs_ && cp.has_program()) {
                programs_->register_program(cp.name(),
                                            cp.program().vert(),
                                            cp.program().frag());
            }
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
    // Stop the worker thread before tearing anything down. Pending
    // ready-but-undrained assets are dropped on the floor here — by
    // construction, OCaml hasn't received a *_loaded event for them
    // yet, so it can't be holding any references that need cleanup.
    if (loader_) {
        loader_->stop();
        loader_.reset();
    }
    walker_.reset();
    programs_.reset();
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
}

void Engine::ship_event(const mlregl::transport::backend::BackendEvent& ev) {
    if (!event_sink_) {
        // Mirror the JS backend, which silently buffers events when no
        // listener is wired. We log once per process so misconfigured
        // setups don't go unnoticed.
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                         "[declgl] ship_event: no EventSink registered "
                         "(events will be dropped); is the bridge wired up?\n");
            warned = true;
        }
        return;
    }
    std::string buf;
    if (!ev.SerializeToString(&buf)) {
        std::fprintf(stderr,
                     "[declgl] ship_event: BackendEvent::SerializeToString failed\n");
        return;
    }
    event_sink_(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
}

void Engine::drain_ready_assets(std::size_t max_items) {
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

    for (auto& r : ready) {
        // ---- failure path (shared between texture / font) ----
        if (!r.error.empty() || !r.image.ok()) {
            std::fprintf(stderr,
                         "[declgl] async load '%s' failed: %s\n",
                         r.name.c_str(),
                         r.error.empty() ? "decode/parse" : r.error.c_str());
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
            using ProtoMin = mlregl::transport::backend::TextureMinOption;
            using ProtoMag = mlregl::transport::backend::TextureMagOption;
            const ProtoMin min_opt = static_cast<ProtoMin>(r.min_filter_enum);
            const ProtoMag mag_opt = static_cast<ProtoMag>(r.mag_filter_enum);
            const bool gen_mipmaps = min_filter_uses_mipmaps(min_opt);

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
                std::fprintf(stderr,
                             "[declgl] async load '%s': upload failed\n",
                             r.name.c_str());
                BackendEvent ev;
                ev.mutable_texture_loadfail()->set_name(r.name);
                ship_event(ev);
                continue;
            }

            const int tw = tex->width();
            const int th = tex->height();
            textures_->register_texture(r.name, std::move(tex));

            BackendEvent ev;
            auto* loaded = ev.mutable_texture_loaded();
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
        if (!tex->upload_rgba8(r.image.width, r.image.height,
                               r.image.pixels.get(),
                               TextureFilter::Linear,
                               TextureFilter::Linear,
                               /*generate_mipmaps=*/false,
                               /*premultiply_alpha=*/false)) {
            std::fprintf(stderr,
                         "[declgl] async load '%s': font atlas upload failed\n",
                         r.name.c_str());
            BackendEvent ev;
            ev.mutable_font_loadfail()->set_name(r.name);
            ship_event(ev);
            continue;
        }
        textures_->register_texture(r.image_url, std::move(tex));

        std::printf("[declgl/font] '%s': %d glyphs, %d kernings, "
                    "%dx%d atlas, lineHeight=%d base=%d range=%.1f\n",
                    r.name.c_str(),
                    r.font->glyph_count(), r.font->kerning_count(),
                    r.font->scaleW(), r.font->scaleH(),
                    r.font->lineHeight(), r.font->base(),
                    r.font->distanceRange());
        fonts_->register_font(r.name, std::move(r.font), r.image_url);

        BackendEvent ev;
        ev.mutable_font_loaded()->set_name(r.name);
        ship_event(ev);
    }
}

void Engine::render(const mlregl::transport::render::Renderable& tree) {
    if (!walker_ || !render_ctx_) return;

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
            if (fbos_) fbos_->resize_all(pw, ph);
        }
    }

    // Mark every palette free for this frame. Acquire/release happens
    // inside the walker as it descends through groups & composites.
    if (fbos_) fbos_->free_all();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    walker_->render(tree, *render_ctx_);
}

}  // namespace declgl
