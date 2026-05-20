// renderer/renderable_walker.cc

#include "renderer/renderable_walker.h"

#include <glad/gl.h>

#include <cstdio>
#include <string_view>
#include <vector>

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

    // Standard built-in uniforms shared by every JS-style program. We
    // tolerate them being absent (some programs don't sample them).
    if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
        glUniform2f(loc, ctx.view_w, ctx.view_h);
    }
    if (const GLint loc = prog->uniform_location("camera"); loc >= 0) {
        glUniform4f(loc, ctx.camera[0], ctx.camera[1],
                         ctx.camera[2], ctx.camera[3]);
    }

    // Field-driven uniforms. Currently we handle the cases used by the
    // M3.B `triangle` program: a `color` field as vec4 uniform, and a
    // `pos` field providing the vertex positions attribute.
    if (const Value* v = find_field(a, "color"); v) {
        const auto vs = as_floats(v);
        if (vs.size() >= 4) {
            if (const GLint loc = prog->uniform_location("color"); loc >= 0) {
                glUniform4f(loc, vs[0], vs[1], vs[2], vs[3]);
            }
        }
    }

    // Position attribute: assume `pos` is a flat array of 2D vertices.
    const Value* posv = find_field(a, "pos");
    const auto positions = as_floats(posv);
    if (positions.empty() || positions.size() % 2 != 0) {
        std::fprintf(stderr,
                     "[declgl/render] '%s': missing/malformed pos field "
                     "(%zu floats)\n",
                     prog_name.c_str(), positions.size());
        return;
    }
    const GLsizei vertex_count =
        static_cast<GLsizei>(positions.size() / 2);

    // One transient VBO + VAO per draw. Inefficient but trivially
    // correct; M3.C will pool these per-program.
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(positions.size() * sizeof(float)),
                 positions.data(), GL_STREAM_DRAW);

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
        glDrawArrays(GL_TRIANGLES, 0, vertex_count);
        glDisableVertexAttribArray(static_cast<GLuint>(pos_loc));
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

void RenderableWalker::render_group(
    const mlregl::transport::render::GroupRenderable& g,
    const RenderContext& ctx) {
    // M3.B: ignore effects + per-group camera; just render children
    // straight onto the current target. Effects + FBO ping-pong come in
    // M3.E.
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
