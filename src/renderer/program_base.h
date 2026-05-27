// renderer/program_base.h — declarative program base class.
//
// Each program inherits from ProgramBase and implements:
//   - name(): program name for registry lookup
//   - vert_source() / frag_source(): GLSL sources (from builtin_shader_source)
//   - prepare(): fill DrawState from runtime input (protobuf fields)
//
// The base class provides:
//   - compile(): compile/link GLSL program, cache uniform/attribute locations
//   - draw(): issue GL calls using prepared DrawState
//   - shared streaming VAO/VBO/EBO for dynamic data

#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "transport_render.pb.h"
#include "renderer/render_context.h"
#include "gpu/program.h"

namespace declgl
{

using ProgramCallField = mlregl::transport::render::ProgramCallField;
using ProgramCallFields = google::protobuf::RepeatedPtrField<ProgramCallField>;

// Helper: find field by key. Returns nullptr if absent.
const ProgramCallField *find_field(const ProgramCallFields &fields,
				   std::string_view key);

// Fixed builtin texture slots used by palette/effect/compositor programs.
// Replaces tiny per-pass unordered_maps in the hot path.
struct BuiltinTextures {
	GLuint tex = 0;
	GLuint fbo = 0;
	GLuint texture = 0;
	GLuint t1 = 0;
	GLuint t2 = 0;
};

// Prepared runtime state for drawing.
// - Dynamic data (from protobuf) is COPIED into vectors (owned)
// - Static data (from program class members) uses raw pointers (not owned)
struct DrawState {
	// Primitive type: GL_TRIANGLES, GL_LINES, etc.
	GLenum primitive = GL_TRIANGLES;

	// Draw mode: indexed (glDrawElements) or array (glDrawArrays)
	bool indexed = false;

	// Vertex count (for array draw) or index count (for indexed draw)
	GLsizei count = 0;

	// --- Attributes ---

	// Dynamic attribute: data copied from runtime input
	struct DynAttrib {
		std::string name;
		GLint loc = -1;
		GLint components = 2; // 2 for vec2, 3 for vec3, 4 for vec4
		std::vector<float> data;
	};
	std::vector<DynAttrib> dyn_attribs;

	// Static attribute: data pointer to program-owned static storage
	// Pointer must remain valid for the duration of draw()
	struct StaticAttrib {
		std::string name;
		GLint loc = -1;
		GLint components = 2;
		const float *data = nullptr;
		GLsizei vertex_count = 0;
	};
	std::vector<StaticAttrib> static_attribs;

	// --- Indices ---

	// Dynamic indices (owned)
	std::vector<uint32_t> indices;

	// Static indices (pointer to program-owned data)
	const uint32_t *static_indices = nullptr;

	// --- Uniforms ---

	struct UniformVal {
		std::string name;
		GLint loc = -1;
		enum class Type { F1, F2, F3, F4, I1, TEX };
		Type type;
		// Stored inline (no heap allocation for small values)
		union {
			float f1;
			float f2[2];
			float f3[3];
			float f4[4];
			int i1;
		};
		GLuint tex_id = 0; // for Type::TEX
	};
	std::vector<UniformVal> uniforms;

	// Add dynamic attribute (copies data)
	void add_dyn_attrib(std::string name, GLint components,
			    const float *data, size_t vertex_count)
	{
		DynAttrib a;
		a.name = std::move(name);
		a.loc = -1;
		a.components = components;
		a.data.assign(data, data + vertex_count * components);
		dyn_attribs.push_back(std::move(a));
	}
	void add_dyn_attrib(GLint loc, GLint components, const float *data,
			    size_t vertex_count)
	{
		DynAttrib a;
		a.loc = loc;
		a.components = components;
		a.data.assign(data, data + vertex_count * components);
		dyn_attribs.push_back(std::move(a));
	}

	// Overload for double data (protobuf NumberArrayValue stores double)
	void add_dyn_attrib(std::string name, GLint components,
			    const double *data, size_t vertex_count)
	{
		DynAttrib a;
		a.name = std::move(name);
		a.loc = -1;
		a.components = components;
		a.data.reserve(vertex_count * components);
		for (size_t i = 0; i < vertex_count * components; ++i) {
			a.data.push_back(static_cast<float>(data[i]));
		}
		dyn_attribs.push_back(std::move(a));
	}
	void add_dyn_attrib(GLint loc, GLint components, const double *data,
			    size_t vertex_count)
	{
		DynAttrib a;
		a.loc = loc;
		a.components = components;
		a.data.reserve(vertex_count * components);
		for (size_t i = 0; i < vertex_count * components; ++i) {
			a.data.push_back(static_cast<float>(data[i]));
		}
		dyn_attribs.push_back(std::move(a));
	}

	// Add static attribute (no copy, pointer must outlive DrawState)
	void add_static_attrib(std::string name, GLint components,
			       const float *data, GLsizei vertex_count)
	{
		static_attribs.push_back({ std::move(name), -1, components,
					   data, vertex_count });
	}
	void add_static_attrib(GLint loc, GLint components, const float *data,
			       GLsizei vertex_count)
	{
		static_attribs.push_back(
			{ {}, loc, components, data, vertex_count });
	}

	// Uniform helpers
	void set_uniform_f1(std::string name, float v)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::F1;
		u.f1 = v;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f1(GLint loc, float v)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::F1;
		u.f1 = v;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f2(std::string name, float x, float y)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::F2;
		u.f2[0] = x;
		u.f2[1] = y;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f2(GLint loc, float x, float y)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::F2;
		u.f2[0] = x;
		u.f2[1] = y;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f3(std::string name, float x, float y, float z)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::F3;
		u.f3[0] = x;
		u.f3[1] = y;
		u.f3[2] = z;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f3(GLint loc, float x, float y, float z)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::F3;
		u.f3[0] = x;
		u.f3[1] = y;
		u.f3[2] = z;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f4(std::string name, float x, float y, float z,
			    float w)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::F4;
		u.f4[0] = x;
		u.f4[1] = y;
		u.f4[2] = z;
		u.f4[3] = w;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f4(GLint loc, float x, float y, float z, float w)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::F4;
		u.f4[0] = x;
		u.f4[1] = y;
		u.f4[2] = z;
		u.f4[3] = w;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_tex(std::string name, GLuint tex_id)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::TEX;
		u.tex_id = tex_id;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_tex(GLint loc, GLuint tex_id)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::TEX;
		u.tex_id = tex_id;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_i1(std::string name, int v)
	{
		UniformVal u;
		u.name = std::move(name);
		u.loc = -1;
		u.type = UniformVal::Type::I1;
		u.i1 = v;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_i1(GLint loc, int v)
	{
		UniformVal u;
		u.loc = loc;
		u.type = UniformVal::Type::I1;
		u.i1 = v;
		uniforms.push_back(std::move(u));
	}
};

// Abstract base class for all declarative programs.
class ProgramBase {
    public:
	virtual ~ProgramBase();

	// --- Program definition (override these) ---

	// Program name for registry lookup
	virtual std::string_view name() const = 0;

	// GLSL sources (typically from builtin_shader_source)
	virtual std::string_view vert_source() const = 0;
	virtual std::string_view frag_source() const = 0;

	// Prepare draw state from runtime input.
	// Returns false to skip this draw (e.g., texture not loaded).
	// ctx provides access to textures, fonts, viewport, camera, etc.
	// builtin_textures contains FBO textures for effects/compositors (t1, t2, texture, fbo).
	virtual bool prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures &builtin_textures,
			     DrawState &out_state) = 0;

	// --- Provided by base class ---

	// Compile and link the GLSL program. Called once by registry.
	// Returns true on success.
	bool compile();

	// Draw using prepared state.
	// Must be called after compile() and prepare().
	void draw(const DrawState &state);

	// Access to underlying GL program
	GLuint gl_program_id() const
	{
		return program_.id();
	}
	const Program &gl_program() const
	{
		return program_;
	}

    protected:
	ProgramBase() = default;
	virtual bool after_compile()
	{
		return true;
	}

	GLint uniform_location(std::string_view name) const
	{
		return cached_uniform_location(name);
	}
	GLint attribute_location(std::string_view name) const
	{
		return cached_attribute_location(name);
	}

	void add_dyn_attrib(DrawState &state, std::string_view name,
			    GLint components, const float *data,
			    size_t vertex_count) const
	{
		state.add_dyn_attrib(attribute_location(name), components, data,
				     vertex_count);
	}
	void add_dyn_attrib(DrawState &state, std::string_view name,
			    GLint components, const double *data,
			    size_t vertex_count) const
	{
		state.add_dyn_attrib(attribute_location(name), components, data,
				     vertex_count);
	}
	void add_static_attrib(DrawState &state, std::string_view name,
			       GLint components, const float *data,
			       GLsizei vertex_count) const
	{
		state.add_static_attrib(attribute_location(name), components,
					data, vertex_count);
	}
	void set_uniform_f1(DrawState &state, std::string_view name,
			    float v) const
	{
		state.set_uniform_f1(uniform_location(name), v);
	}
	void set_uniform_f2(DrawState &state, std::string_view name, float x,
			    float y) const
	{
		state.set_uniform_f2(uniform_location(name), x, y);
	}
	void set_uniform_f3(DrawState &state, std::string_view name, float x,
			    float y, float z) const
	{
		state.set_uniform_f3(uniform_location(name), x, y, z);
	}
	void set_uniform_f4(DrawState &state, std::string_view name, float x,
			    float y, float z, float w) const
	{
		state.set_uniform_f4(uniform_location(name), x, y, z, w);
	}
	void set_uniform_i1(DrawState &state, std::string_view name,
			    int v) const
	{
		state.set_uniform_i1(uniform_location(name), v);
	}
	void set_uniform_tex(DrawState &state, std::string_view name,
			     GLuint tex_id) const
	{
		state.set_uniform_tex(uniform_location(name), tex_id);
	}

	// Helper: set built-in uniforms (view, camera) from ctx
	void set_builtin_uniforms(const RenderContext &ctx, DrawState &state)
	{
		set_uniform_f2(state, "view", ctx.view_w * 0.5f,
			       -ctx.view_h * 0.5f);
		set_uniform_f4(state, "camera", ctx.camera[0], ctx.camera[1],
			       ctx.camera[2], ctx.camera[3]);
	}

    private:
	Program program_; // compiled GLSL program

	struct StaticAttribBuffer {
		const float *data = nullptr;
		GLsizei vertex_count = 0;
		GLint components = 0;
		GLuint buffer = 0;
	};
	struct StaticIndexBuffer {
		const uint32_t *data = nullptr;
		GLsizei count = 0;
		GLuint buffer = 0;
	};
	struct LocationCacheEntry {
		std::string name;
		GLint loc = -1;
	};

	GLuint vao_ = 0;
	GLuint dynamic_vbo_ = 0;
	GLuint dynamic_ebo_ = 0;
	std::vector<GLuint> enabled_attribs_;
	std::vector<StaticAttribBuffer> static_attrib_buffers_;
	std::vector<StaticIndexBuffer> static_index_buffers_;
	mutable std::vector<LocationCacheEntry> uniform_location_cache_;
	mutable std::vector<LocationCacheEntry> attribute_location_cache_;

	void ensure_program_buffers();
	GLuint static_attrib_buffer(const DrawState::StaticAttrib &a);
	GLuint static_index_buffer(const uint32_t *data, GLsizei count);
	GLint cached_uniform_location(std::string_view name) const;
	GLint cached_attribute_location(std::string_view name) const;

	// Global last program bound (for caching)
	static GLuint &last_program_bound();
};

} // namespace declgl
