// renderer/renderable_walker.cc

#include "renderer/renderable_walker.h"

#include <glad/gl.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/fbo_pool.h"
#include "renderer/render_context.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl {

namespace {

using mlregl::transport::common::Value;
using mlregl::transport::render::AtomicRenderable;
using mlregl::transport::render::ProgramCallField;

// Lookup a field by key. Returns nullptr if absent.
const Value* find_field(const AtomicRenderable& a, std::string_view key) {
    for (const auto& f : a.fields()) {
        if (f.key() == key) return f.has_val() ? &f.val() : nullptr;
    }
    return nullptr;
}

// Coerce a Value to a vector<float>. Number→[n], NumberArray→[..], else {}.
std::vector<float> as_floats(const Value* v) {
    std::vector<float> out;
    if (!v) return out;
    switch (v->kind_case()) {
        case Value::kNumberValue:
            out.push_back(static_cast<float>(v->number_value()));
            break;
        case Value::kNumberArrayValue:
            for (double d : v->number_array_value().values())
                out.push_back(static_cast<float>(d));
            break;
        default:
            break;
    }
    return out;
}

// Map JS-side primitive name strings (as defined in ml-regl-core) to a
// GL primitive enum. Defaults to GL_TRIANGLES for unknown / absent.
GLenum primitive_from_string(const std::string& s) {
    if (s == "points")          return GL_POINTS;
    if (s == "lines")           return GL_LINES;
    if (s == "line strip")      return GL_LINE_STRIP;
    if (s == "line loop")       return GL_LINE_LOOP;
    if (s == "triangles")       return GL_TRIANGLES;
    if (s == "triangle strip")  return GL_TRIANGLE_STRIP;
    if (s == "triangle fan")    return GL_TRIANGLE_FAN;
    return GL_TRIANGLES;
}

// Fullscreen NDC unit-quad geometry used by programs that don't ship a
// per-call `pos` field (rect, circle, roundedRect). The exact corners
// depend on the program: rect uses [0..1]² (its vert subtracts 0.5 then
// scales by `posize`), while circle/roundedRect use [-1..1]² so the
// vert can hand world-space coords through `v_position`.
struct QuadGeom {
    std::array<float, 8> verts;          // 4 corners, x/y interleaved
    std::array<uint32_t, 6> indices;     // two triangles, [0,1,2,0,2,3]
};

const QuadGeom* hardcoded_quad_for(std::string_view program) {
    static constexpr QuadGeom kRectQuad = {
        // [0,1]² — matches ml-regl-js/src/rect/vert.glsl convention
        { 0.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f },
        { 0u, 1u, 2u,  0u, 2u, 3u },
    };
    static constexpr QuadGeom kNdcQuad  = {
        // [-1,1]² — matches ml-regl-js/src/circle/vert.glsl convention
        { -1.f, -1.f,  1.f, -1.f,  1.f, 1.f,  -1.f, 1.f },
        { 0u, 1u, 2u,  0u, 2u, 3u },
    };
    if (program == "rect")        return &kRectQuad;
    if (program == "circle")      return &kNdcQuad;
    if (program == "roundedRect") return &kNdcQuad;
    return nullptr;
}

// True for the four texture-sampling builtins. Captured here so the
// branchy texture path stays out of the hot path for non-textured
// draws (most calls in a typical scene).
bool is_texture_program(std::string_view program) {
    return program == "texture"
        || program == "textureCropped"
        || program == "centeredTexture"
        || program == "centeredCroppedTexture";
}

// True iff this textured program ships its own `texc` attribute as a
// per-call field. The other two get the JS-hardcoded UV layout.
bool texture_program_uses_caller_texc(std::string_view program) {
    return program == "textureCropped" || program == "centeredCroppedTexture";
}

// Which programs need the indices-only hardcoded fallback. Both
// textureCropped and texture ship `pos` themselves but reuse the JS
// 6-index quad layout. centeredTexture/centeredCroppedTexture ship
// neither pos nor indices.
const std::array<uint32_t, 6>& texture_quad_indices() {
    static constexpr std::array<uint32_t, 6> kIdx = { 0u, 1u, 2u,  0u, 2u, 3u };
    return kIdx;
}

// JS-hardcoded `texc` table for [texture] and [centeredTexture]. The
// (Y-flipped) corners drive each quad's UV plumbing in the vertex
// shaders we vendored.
const std::array<float, 8>& texture_default_texc() {
    static constexpr std::array<float, 8> kTexc = {
        0.f, 1.f,
        1.f, 1.f,
        1.f, 0.f,
        0.f, 0.f,
    };
    return kTexc;
}

// JS-hardcoded `texc2` table for [centeredCroppedTexture]. Drives the
// per-vertex unit-quad corner used to fan out posize.zw.
const std::array<float, 8>& centered_cropped_texc2() {
    static constexpr std::array<float, 8> kTexc2 = {
        -0.5f, 0.5f,
         0.5f, 0.5f,
         0.5f, -0.5f,
        -0.5f, -0.5f,
    };
    return kTexc2;
}

// Expand the OCaml's 4-float `texc` (cx, cy, cw, ch) into 8-float
// per-corner UVs. Mirrors the preprocessor in
// ml-regl-js/src/app.js for [centered_texture_cropped]:
//
//     [x1,y1, x1+w,y1, x1+w,y1+h, x1,y1+h]
//
// We do this in the walker because the JS-side closure that does the
// expansion is JS-only — the OCaml builder ships the compact 4-tuple
// directly.
std::array<float, 8> expand_centered_cropped_texc(const std::vector<float>& v) {
    if (v.size() < 4) return texture_default_texc();
    const float x1 = v[0], y1 = v[1], w = v[2], h = v[3];
    return {
        x1,     y1,
        x1 + w, y1,
        x1 + w, y1 + h,
        x1,     y1 + h,
    };
}

// Set a uniform from a Value, choosing the glUniformNfv flavour from
// the array length. Silently no-ops if the program doesn't have that
// uniform (location < 0).
void set_uniform_from_value(const Program& prog,
                            std::string_view uniform_name,
                            const Value& v) {
    const GLint loc = prog.uniform_location(uniform_name);
    if (loc < 0) return;

    auto vs = as_floats(&v);
    switch (vs.size()) {
        case 1: glUniform1f(loc, vs[0]); break;
        case 2: glUniform2f(loc, vs[0], vs[1]); break;
        case 3: glUniform3f(loc, vs[0], vs[1], vs[2]); break;
        case 4: glUniform4f(loc, vs[0], vs[1], vs[2], vs[3]); break;
        default: /* scalar non-number, or > 4 — ignore */ break;
    }
}

}  // namespace

RenderableWalker::~RenderableWalker() {
    if (fs_ebo_) glDeleteBuffers(1, &fs_ebo_);
    if (fs_vbo_) glDeleteBuffers(1, &fs_vbo_);
    if (fs_vao_) glDeleteVertexArrays(1, &fs_vao_);
}

void RenderableWalker::render(const mlregl::transport::render::Renderable& r,
                              const RenderContext& ctx) {
    // Snapshot the framebuffer the engine had bound on entry. The
    // top-level palette is blitted back to it via the `palette`
    // program, mirroring JS step():
    //   const pid = drawRenderable(gview);
    //   if (pid >= 0) drawPalette({ fbo: fbos[pid] });
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    target_fbo_at_entry_ = prev_fbo;

    // Forward-rendering fast path: no FBO pool means we can't allocate
    // palettes, so just render directly to the bound framebuffer. This
    // is always safe for trees that contain only atomics with no
    // effects — the visible output is identical because there's
    // nothing to ping-pong through.
    if (!ctx.fbos || ctx.fbos->size() == 0) {
        switch (r.kind_case()) {
            using R = mlregl::transport::render::Renderable;
            case R::kAtomic:    render_atomic(r.atomic(), ctx); break;
            case R::kGroup:
                for (const auto& child : r.group().children())
                    render(child, ctx);
                break;
            case R::kComposite:
                if (r.composite().has_left())
                    render(r.composite().left(),  ctx);
                if (r.composite().has_right())
                    render(r.composite().right(), ctx);
                break;
            default: break;
        }
        return;
    }

    const int pid = draw_renderable(r, ctx);
    if (pid < 0) return;

    // Restore the entry framebuffer and blit the result palette via the
    // [palette] passthrough program.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(0, 0, ctx.pixel_w, ctx.pixel_h);
    if (const Program* prog = programs_.get("palette")) {
        glUseProgram(prog->id());
        bind_palette_sampler(*prog, "tex", pid, 0, ctx);
        draw_fullscreen_quad(*prog);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    release_pid(pid, ctx);
}

void RenderableWalker::render_atomic(const AtomicRenderable& a,
                                     const RenderContext& ctx) {
    const std::string& prog_name = a.program();

    // Special case: "clear" is not a draw, it's just glClear with a
    // chosen colour (and an optional depth). Mirrors the JS backend
    // `regl.clear`.
    if (prog_name == "clear") {
        const auto col = as_floats(find_field(a, "color"));
        if (col.size() >= 4) {
            glClearColor(col[0], col[1], col[2], col[3]);
        } else {
            glClearColor(0.f, 0.f, 0.f, 1.f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    const Program* prog = programs_.get(prog_name);
    if (!prog) {
        // Once per missing program, not per frame.
        static std::string last;
        if (last != prog_name) {
            std::fprintf(stderr, "[declgl/render] no program '%s'\n",
                         prog_name.c_str());
            last = prog_name;
        }
        return;
    }

    glUseProgram(prog->id());

    // Standard built-in uniforms shared by every JS-style program.
    // Tolerated as absent (some programs don't sample them).
    //
    // The `view` uniform is NOT the raw canvas size: it's
    // `(virtWidth/2, -virtHeight/2)`. Two reasons, both inherited
    // verbatim from ml-regl-js/src/app.js:
    //   1. dividing by half-canvas maps the full canvas to NDC ±1
    //      (the shaders compute `pos = diff * camera.z / view`).
    //   2. negating Y converts screen-space (Y down, origin top-left)
    //      to GL NDC (Y up, origin bottom-left) without any extra
    //      shader logic — same shaders work for both backends.
    if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
        glUniform2f(loc, ctx.view_w * 0.5f, -ctx.view_h * 0.5f);
    }
    if (const GLint loc = prog->uniform_location("camera"); loc >= 0) {
        glUniform4f(loc, ctx.camera[0], ctx.camera[1],
                         ctx.camera[2], ctx.camera[3]);
    }

    // Field-driven uniforms. We try to bind every numeric field as a
    // uniform of the same name on the program; the program object
    // returns -1 for unknown names so unused fields are harmless. This
    // covers `color` (vec4), `posize` (vec4), `angle` (float), `cr`
    // (vec3), `cs` (vec4), `radius` (float), `depth` (float), `alpha`
    // (float), and any future scalars/short vectors a builtin might
    // want.
    //
    // Structural fields are excluded:
    //   - `pos`, `elem`, `prim` are geometry; handled below.
    //   - `texture` is a string sampler binding; resolved later.
    //   - `texc` / `texc2` are vertex attributes, not uniforms.
    bool has_alpha_field = false;
    for (const auto& f : a.fields()) {
        const auto& key = f.key();
        if (!f.has_val()) continue;
        if (key == "pos" || key == "elem" || key == "prim" ||
            key == "texture" || key == "texc" || key == "texc2") {
            continue;
        }
        if (key == "alpha") has_alpha_field = true;
        set_uniform_from_value(*prog, key, f.val());
    }

    // Default `alpha` = 1.0 if the caller didn't provide it. The JS
    // backend has the same fallback in its preprocessors. Only
    // touched when the program actually has an `alpha` uniform.
    if (!has_alpha_field) {
        if (const GLint loc = prog->uniform_location("alpha"); loc >= 0) {
            glUniform1f(loc, 1.0f);
        }
    }

    // ----------------------------------------------------------------
    // Texture path (M3.D).
    //
    // The four sampling builtins all share this routine: bind the
    // named texture to TEXUNIT0, source/synthesize the per-vertex
    // `position`, `texc` and (for the cropped-centered variant) `texc2`
    // streams per the JS app.js setup, then draw the 6-index quad.
    //
    // Kept separate from the general field-driven path because
    // textures need (a) a non-numeric field (`texture` string) which
    // the generic uniform binder can't translate, (b) extra attribute
    // VBOs that vary per-program, and (c) a sampler-uniform
    // assignment (`tex` on TEXUNIT0).
    // ----------------------------------------------------------------
    if (is_texture_program(prog_name)) {
        // 1. Resolve and bind the texture.
        const Value* tex_v = find_field(a, "texture");
        if (!tex_v || tex_v->kind_case() != Value::kStringValue) {
            std::fprintf(stderr,
                         "[declgl/render] '%s': missing or non-string "
                         "`texture` field\n", prog_name.c_str());
            return;
        }
        if (!ctx.textures) {
            std::fprintf(stderr,
                         "[declgl/render] '%s': no TextureRegistry on "
                         "context; cannot resolve '%s'\n",
                         prog_name.c_str(),
                         tex_v->string_value().c_str());
            return;
        }
        const Texture* tex = ctx.textures->get(tex_v->string_value());
        if (!tex) {
            // Mirror the JS preprocessor: silently drop draws targeting
            // not-yet-loaded textures. The OCaml side already handles
            // load_texture asynchronously, so this is the normal
            // first-frame condition for any textured asset.
            return;
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex->id());
        if (const GLint loc = prog->uniform_location("tex"); loc >= 0) {
            glUniform1i(loc, 0);
        }

        // 2. Resolve `position` / `texc` / `texc2` source streams.
        std::vector<float>      pos_buf;
        const float*            positions     = nullptr;
        GLsizei                 vertex_count  = 4;

        std::vector<float>      texc_buf;
        const float*            texc_data     = nullptr;
        std::array<float, 8>    texc_storage{};

        const float*            texc2_data    = nullptr;

        if (prog_name == "centeredTexture" ||
            prog_name == "centeredCroppedTexture") {
            // Vertex shader synthesizes positions from `texc` + `posize`,
            // so there's no `position` attribute to feed.
            positions    = nullptr;
            vertex_count = 4;
        } else {
            // `texture` and `textureCropped` source `position` from `pos`.
            pos_buf = as_floats(find_field(a, "pos"));
            if (pos_buf.size() < 8 || pos_buf.size() % 2 != 0) {
                std::fprintf(stderr,
                             "[declgl/render] '%s': `pos` must have at "
                             "least 4 vertices (got %zu floats)\n",
                             prog_name.c_str(), pos_buf.size());
                return;
            }
            positions    = pos_buf.data();
            vertex_count = static_cast<GLsizei>(pos_buf.size() / 2);
        }

        if (texture_program_uses_caller_texc(prog_name)) {
            texc_buf = as_floats(find_field(a, "texc"));
            if (prog_name == "centeredCroppedTexture") {
                // 4-float (cx,cy,cw,ch) → 8-float per-corner expansion.
                texc_storage = expand_centered_cropped_texc(texc_buf);
                texc_data    = texc_storage.data();
            } else {
                if (texc_buf.size() < 8) {
                    std::fprintf(stderr,
                                 "[declgl/render] '%s': caller-supplied "
                                 "`texc` must have 8 floats (got %zu)\n",
                                 prog_name.c_str(), texc_buf.size());
                    return;
                }
                texc_data = texc_buf.data();
            }
        } else {
            texc_data = texture_default_texc().data();
        }

        if (prog_name == "centeredCroppedTexture") {
            texc2_data = centered_cropped_texc2().data();
        }

        // 3. Build the VAO with up to three attribute streams + EBO.
        const auto& idx = texture_quad_indices();
        GLuint vao = 0, vbo_pos = 0, vbo_texc = 0, vbo_texc2 = 0, ebo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        auto bind_vec2_attrib = [&](const char* name, const float* data,
                                    int count, GLuint& vbo_out) {
            const GLint loc = prog->attribute_location(name);
            if (loc < 0 || data == nullptr) return;
            glGenBuffers(1, &vbo_out);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_out);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(count * 2 * sizeof(float)),
                         data, GL_STREAM_DRAW);
            glEnableVertexAttribArray(static_cast<GLuint>(loc));
            glVertexAttribPointer(static_cast<GLuint>(loc), 2, GL_FLOAT,
                                  GL_FALSE, 0, nullptr);
        };

        bind_vec2_attrib("position", positions,  vertex_count, vbo_pos);
        bind_vec2_attrib("texc",     texc_data,  vertex_count, vbo_texc);
        bind_vec2_attrib("texc2",    texc2_data, vertex_count, vbo_texc2);

        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)),
                     idx.data(), GL_STREAM_DRAW);

        glDrawElements(GL_TRIANGLES,
                       static_cast<GLsizei>(idx.size()),
                       GL_UNSIGNED_INT, nullptr);

        // Tear down. VBO/EBO deletes detach automatically from the VAO
        // we're about to delete; doing it in this order keeps GL state
        // explicit and matches the rest of the walker.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        if (vbo_pos)   glDeleteBuffers(1, &vbo_pos);
        if (vbo_texc)  glDeleteBuffers(1, &vbo_texc);
        if (vbo_texc2) glDeleteBuffers(1, &vbo_texc2);
        glDeleteBuffers(1, &ebo);
        glDeleteVertexArrays(1, &vao);

        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // Geometry assembly.
    //
    // Three sources of vertex positions, in priority order:
    //   1. an explicit `pos` field (triangle, quad, poly, lines, ...)
    //   2. a hardcoded unit/NDC quad for programs that fix their own
    //      geometry (rect, circle, roundedRect)
    //   3. neither => skip silently.
    const Value* pos_v = find_field(a, "pos");
    std::vector<float> caller_positions = as_floats(pos_v);

    const float*     positions     = nullptr;
    GLsizei          vertex_count  = 0;
    const uint32_t*  indices       = nullptr;
    GLsizei          index_count   = 0;
    std::vector<uint32_t> caller_indices;

    if (!caller_positions.empty()) {
        if (caller_positions.size() % 2 != 0) {
            std::fprintf(stderr,
                         "[declgl/render] '%s': malformed pos field "
                         "(%zu floats — not even)\n",
                         prog_name.c_str(), caller_positions.size());
            return;
        }
        positions    = caller_positions.data();
        vertex_count = static_cast<GLsizei>(caller_positions.size() / 2);

        // Optional caller-supplied element indices.
        if (const Value* ev = find_field(a, "elem"); ev) {
            const auto efs = as_floats(ev);
            caller_indices.reserve(efs.size());
            for (float e : efs) caller_indices.push_back(static_cast<uint32_t>(e));
            indices     = caller_indices.data();
            index_count = static_cast<GLsizei>(caller_indices.size());
        }
    } else if (const QuadGeom* g = hardcoded_quad_for(prog_name); g) {
        positions    = g->verts.data();
        vertex_count = 4;
        indices      = g->indices.data();
        index_count  = 6;
    } else {
        // No geometry at all and no hardcoded fallback — nothing to draw.
        static std::string last;
        if (last != prog_name) {
            std::fprintf(stderr,
                         "[declgl/render] '%s': no `pos` field and no "
                         "hardcoded geometry; skipping\n",
                         prog_name.c_str());
            last = prog_name;
        }
        return;
    }

    // Optional primitive-kind override (used by `lines`, `linestrip`,
    // `lineloop`, `function_curve`, ...).
    GLenum prim = GL_TRIANGLES;
    if (const Value* pv = find_field(a, "prim"); pv && pv->kind_case() == Value::kStringValue) {
        prim = primitive_from_string(pv->string_value());
    }

    // One transient VAO/VBO/EBO per draw. Pooling is M3.D's job.
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    if (index_count > 0) glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertex_count * 2 * sizeof(float)),
                 positions, GL_STREAM_DRAW);

    const GLint pos_loc = prog->attribute_location("position");
    if (pos_loc < 0) {
        std::fprintf(stderr,
                     "[declgl/render] '%s': vertex shader has no "
                     "'position' attribute\n",
                     prog_name.c_str());
    } else {
        glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
        glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT,
                              GL_FALSE, 0, nullptr);

        if (index_count > 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(index_count * sizeof(uint32_t)),
                         indices, GL_STREAM_DRAW);
            glDrawElements(prim, index_count, GL_UNSIGNED_INT, nullptr);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        } else {
            glDrawArrays(prim, 0, vertex_count);
        }

        glDisableVertexAttribArray(static_cast<GLuint>(pos_loc));
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    glDeleteVertexArrays(1, &vao);
}

void RenderableWalker::release_pid(int pid, const RenderContext& ctx) {
    if (pid >= 0 && ctx.fbos) ctx.fbos->release(pid);
}

void RenderableWalker::bind_fbo(int pid, const RenderContext& ctx) {
    if (pid < 0 || !ctx.fbos) {
        glBindFramebuffer(GL_FRAMEBUFFER,
                          static_cast<GLuint>(target_fbo_at_entry_));
        glViewport(0, 0, ctx.pixel_w, ctx.pixel_h);
        return;
    }
    const Fbo* f = ctx.fbos->get(pid);
    if (!f) return;
    glBindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
    glViewport(0, 0, f->width, f->height);
}

void RenderableWalker::bind_palette_sampler(const Program& prog,
                                            std::string_view uniform_name,
                                            int pid, int unit,
                                            const RenderContext& ctx) {
    if (!ctx.fbos) return;
    const Fbo* f = ctx.fbos->get(pid);
    if (!f) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, f->texture);
    if (const GLint loc = prog.uniform_location(uniform_name); loc >= 0) {
        glUniform1i(loc, unit);
    }
}

void RenderableWalker::draw_fullscreen_quad(const Program& prog) {
    // Lazy build: shared across every effect & compositor draw. The
    // attribute layout (texc only) matches what [effect.vert.glsl]
    // expects; the corner ordering matches the JS regl spec
    // (`drawPalette` and friends in app.js):
    //   [1,1,  1,0,  0,0,  0,1]
    if (!fs_built_) {
        static constexpr float kTexc[8] = {
            1.f, 1.f,
            1.f, 0.f,
            0.f, 0.f,
            0.f, 1.f,
        };
        static constexpr uint32_t kIdx[6] = { 0, 1, 2, 0, 2, 3 };

        glGenVertexArrays(1, &fs_vao_);
        glGenBuffers(1, &fs_vbo_);
        glGenBuffers(1, &fs_ebo_);

        glBindVertexArray(fs_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, fs_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kTexc), kTexc, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fs_ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIdx), kIdx, GL_STATIC_DRAW);
        // Note: the `texc` attribute location may differ per program
        // (effect/palette/etc all share effect.vert though, so they
        // tend to match), but glVertexAttribPointer is bound per-VAO
        // by the active attrib *index*, not name. We therefore
        // re-establish the pointer below for whichever program is
        // active, leaving the data in this VAO's VBO unchanged.
        glBindVertexArray(0);
        fs_built_ = true;
    }

    glBindVertexArray(fs_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, fs_vbo_);
    const GLint loc = prog.attribute_location("texc");
    if (loc >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), 2, GL_FLOAT,
                              GL_FALSE, 0, nullptr);
    }
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    if (loc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(loc));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

int RenderableWalker::draw_renderable(
    const mlregl::transport::render::Renderable& r,
    const RenderContext& ctx) {
    using R = mlregl::transport::render::Renderable;
    switch (r.kind_case()) {
        case R::kAtomic: {
            // JS [drawRenderable]: solo atomic gets its own palette,
            // cleared, drawn into.
            const int pid = ctx.fbos->acquire();
            if (pid < 0) return -1;
            bind_fbo(pid, ctx);
            glClearColor(0.f, 0.f, 0.f, 0.f);
            glClear(GL_COLOR_BUFFER_BIT);
            render_atomic(r.atomic(), ctx);
            return pid;
        }
        case R::kGroup:
            return draw_group(r.group(), -1, ctx);
        case R::kComposite:
            return draw_composite(r.composite(), ctx);
        default:
            return -1;
    }
}

int RenderableWalker::draw_group(
    const mlregl::transport::render::GroupRenderable& g,
    int prev_pid, const RenderContext& ctx) {
    if (g.children_size() == 0) return prev_pid;

    // Camera scoping: caller-save / callee-restore. This mirrors JS
    // drawGroup which does `let prev_camera = camera; ... camera = prev_camera;`.
    // We use a per-call RenderContext copy for cleanliness.
    RenderContext child_ctx = ctx;
    if (g.has_camera()) {
        const auto& c = g.camera();
        child_ctx.camera = {
            static_cast<float>(c.x()), static_cast<float>(c.y()),
            static_cast<float>(c.zoom()), static_cast<float>(c.rotation())
        };
    }

    int cur_pid = prev_pid;
    int i = 0;
    const int n = g.children_size();
    using R = mlregl::transport::render::Renderable;
    while (i < n) {
        const auto& c = g.children(i);
        switch (c.kind_case()) {
            case R::kGroup: {
                // Effect-bearing nested groups break the batch and
                // start fresh; effect-free ones inherit our palette.
                const bool nested_has_effects =
                    c.group().effects_size() > 0;
                const int sub = draw_group(
                    c.group(),
                    nested_has_effects ? -1 : cur_pid,
                    child_ctx);
                cur_pid = simple_compose(cur_pid, sub, child_ctx);
                ++i;
                break;
            }
            case R::kComposite: {
                const int sub = draw_composite(c.composite(), child_ctx);
                cur_pid = simple_compose(cur_pid, sub, child_ctx);
                ++i;
                break;
            }
            case R::kAtomic: {
                // Atomic batching: while consecutive children are
                // atomics, draw them all into the same palette to
                // avoid one acquire/release per atomic. This matches
                // JS drawGroup's inner `while (i < cmds.length)` loop.
                const bool fresh_palette = (cur_pid < 0);
                if (fresh_palette) {
                    cur_pid = ctx.fbos->acquire();
                    if (cur_pid < 0) { ++i; break; }
                }
                bind_fbo(cur_pid, child_ctx);
                if (fresh_palette) {
                    glClearColor(0.f, 0.f, 0.f, 0.f);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
                while (i < n) {
                    const auto& lc = g.children(i);
                    if (lc.kind_case() != R::kAtomic) break;
                    render_atomic(lc.atomic(), child_ctx);
                    ++i;
                }
                break;
            }
            default:
                ++i;
                break;
        }
    }

    // Apply effects in declaration order. Each one ping-pongs into a
    // fresh palette and frees the previous one. JS:
    //   curPalette = applyEffect(e, curPalette); freePID(curPalette_old);
    for (int ei = 0; ei < g.effects_size(); ++ei) {
        if (cur_pid < 0) break;  // empty group, nothing to fade
        const int npid = apply_effect(g.effects(ei), cur_pid, child_ctx);
        release_pid(cur_pid, ctx);
        cur_pid = npid;
    }

    return cur_pid;
}

int RenderableWalker::draw_composite(
    const mlregl::transport::render::CompositeRenderable& c,
    const RenderContext& ctx) {
    if (!c.has_compositor()) return -1;
    const int r1 = c.has_left()  ? draw_renderable(c.left(),  ctx) : -1;
    const int r2 = c.has_right() ? draw_renderable(c.right(), ctx) : -1;
    if (r1 < 0 && r2 < 0) return -1;

    const int npid = ctx.fbos->acquire();
    if (npid < 0) {
        release_pid(r1, ctx);
        release_pid(r2, ctx);
        return -1;
    }

    const auto& comp = c.compositor();
    const Program* prog = programs_.get(comp.program());
    if (!prog) {
        // Compositor program missing: best-effort fallback is to flush
        // either half straight to the new palette so something
        // visible shows up. We pick the left half (matches the JS
        // default-compositor behaviour at mode=0).
        bind_fbo(npid, ctx);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (r1 >= 0) {
            if (const Program* pal = programs_.get("palette")) {
                glUseProgram(pal->id());
                bind_palette_sampler(*pal, "tex", r1, 0, ctx);
                draw_fullscreen_quad(*pal);
            }
        }
        release_pid(r1, ctx);
        release_pid(r2, ctx);
        return npid;
    }

    bind_fbo(npid, ctx);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog->id());

    // Bind both source palettes as samplers.
    bind_palette_sampler(*prog, "t1", r1, 0, ctx);
    bind_palette_sampler(*prog, "t2", r2, 1, ctx);

    // Numeric/scalar fields → uniforms (e.g. `t` for compFade,
    // `mode` for defaultCompositor).
    for (const auto& f : comp.fields()) {
        if (!f.has_val()) continue;
        const auto& key = f.key();
        if (key == "t1" || key == "t2") continue;  // sampler bindings
        // `mode` is an int — special-case integer uniform binding so
        // it doesn't get coerced to float (which a sampler/int uniform
        // wouldn't accept and would log a GL_INVALID_OPERATION).
        if (key == "mode") {
            if (f.val().kind_case() == Value::kNumberValue) {
                if (const GLint loc = prog->uniform_location("mode"); loc >= 0) {
                    glUniform1i(loc, static_cast<GLint>(f.val().number_value()));
                }
            }
            continue;
        }
        set_uniform_from_value(*prog, key, f.val());
    }

    draw_fullscreen_quad(*prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    release_pid(r1, ctx);
    release_pid(r2, ctx);
    return npid;
}

int RenderableWalker::simple_compose(int old_pid, int new_pid,
                                     const RenderContext& ctx) {
    if (old_pid < 0)         return new_pid;
    if (new_pid < 0)         return old_pid;
    if (old_pid == new_pid)  return old_pid;

    bind_fbo(old_pid, ctx);
    if (const Program* prog = programs_.get("palette")) {
        glUseProgram(prog->id());
        bind_palette_sampler(*prog, "tex", new_pid, 0, ctx);
        draw_fullscreen_quad(*prog);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    release_pid(new_pid, ctx);
    return old_pid;
}

int RenderableWalker::apply_effect(
    const mlregl::transport::render::Effect& e,
    int src_pid, const RenderContext& ctx) {
    const int npid = ctx.fbos->acquire();
    if (npid < 0) return src_pid;  // pool exhausted, drop the effect

    const Program* prog = programs_.get(e.program());
    if (!prog) {
        // Unknown effect program — fall back to a passthrough.
        bind_fbo(npid, ctx);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (const Program* pal = programs_.get("palette")) {
            glUseProgram(pal->id());
            bind_palette_sampler(*pal, "tex", src_pid, 0, ctx);
            draw_fullscreen_quad(*pal);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        return npid;
    }

    bind_fbo(npid, ctx);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog->id());
    bind_palette_sampler(*prog, "tex", src_pid, 0, ctx);

    // Numeric fields → uniforms by name. `view` is provided too
    // (some effects like blur scale by it). The `texture` field key
    // would normally be string and is reserved for the sampler; the
    // walker doesn't treat it specially here because effects don't
    // ship one — the source FBO is auto-bound.
    if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
        glUniform2f(loc, ctx.view_w * 0.5f, -ctx.view_h * 0.5f);
    }
    for (const auto& f : e.fields()) {
        if (!f.has_val()) continue;
        const auto& key = f.key();
        if (key == "texture" || key == "tex") continue;
        set_uniform_from_value(*prog, key, f.val());
    }

    draw_fullscreen_quad(*prog);
    glBindTexture(GL_TEXTURE_2D, 0);
    return npid;
}

}  // namespace declgl

