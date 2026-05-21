// renderer/renderable_walker.cc

#include "renderer/renderable_walker.h"

#include <glad/gl.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

void RenderableWalker::render(const mlregl::transport::render::Renderable& r,
                              const RenderContext& ctx) {
    using mlregl::transport::render::Renderable;
    switch (r.kind_case()) {
        case Renderable::kAtomic:    render_atomic(r.atomic(), ctx);       break;
        case Renderable::kGroup:     render_group(r.group(), ctx);         break;
        case Renderable::kComposite: render_composite(r.composite(), ctx); break;
        case Renderable::KIND_NOT_SET:
        default: break;
    }
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

void RenderableWalker::render_group(
    const mlregl::transport::render::GroupRenderable& g,
    const RenderContext& ctx) {
    // M3.B/M3.C: ignore effects; just propagate camera and recurse.
    // Effects + FBO ping-pong come in M3.E.
    RenderContext child_ctx = ctx;
    if (g.has_camera()) {
        const auto& c = g.camera();
        child_ctx.camera = {
            static_cast<float>(c.x()), static_cast<float>(c.y()),
            static_cast<float>(c.zoom()), static_cast<float>(c.rotation())
        };
    }
    for (const auto& child : g.children()) render(child, child_ctx);
}

void RenderableWalker::render_composite(
    const mlregl::transport::render::CompositeRenderable& c,
    const RenderContext& ctx) {
    // M3.B placeholder — proper compositing requires FBO ping-pong.
    // For now, render both halves onto the current target (left first,
    // then right on top). Compositor program is ignored.
    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr,
                     "[declgl/render] CompositeRenderable seen — "
                     "compositing not implemented (M3.E); rendering "
                     "left+right flat\n");
        warned = true;
    }
    if (c.has_left())  render(c.left(),  ctx);
    if (c.has_right()) render(c.right(), ctx);
}

}  // namespace declgl
