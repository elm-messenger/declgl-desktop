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
#include <unordered_map>

#include "transport_render.pb.h"
#include "renderer/render_context.h"
#include "gpu/program.h"

namespace declgl
{

using ProgramCallField = mlregl::transport::render::ProgramCallField;
using ProgramCallFields =
	google::protobuf::RepeatedPtrField<ProgramCallField>;

// Helper: find field by key. Returns nullptr if absent.
const ProgramCallField *find_field(const ProgramCallFields &fields,
				   std::string_view key);

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
		GLint components = 2; // 2 for vec2, 3 for vec3, 4 for vec4
		std::vector<float> data;
	};
	std::vector<DynAttrib> dyn_attribs;

	// Static attribute: data pointer to program-owned static storage
	// Pointer must remain valid for the duration of draw()
	struct StaticAttrib {
		std::string name;
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

	// --- Helpers ---

	// Add dynamic attribute (copies data)
	void add_dyn_attrib(std::string name, GLint components,
			    const float *data, size_t vertex_count)
	{
		DynAttrib a;
		a.name = std::move(name);
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
		static_attribs.push_back(
			{ std::move(name), components, data, vertex_count });
	}

	// Uniform helpers
	void set_uniform_f1(std::string name, float v)
	{
		UniformVal u;
		u.name = std::move(name);
		u.type = UniformVal::Type::F1;
		u.f1 = v;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f2(std::string name, float x, float y)
	{
		UniformVal u;
		u.name = std::move(name);
		u.type = UniformVal::Type::F2;
		u.f2[0] = x;
		u.f2[1] = y;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f3(std::string name, float x, float y, float z)
	{
		UniformVal u;
		u.name = std::move(name);
		u.type = UniformVal::Type::F3;
		u.f3[0] = x;
		u.f3[1] = y;
		u.f3[2] = z;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_f4(std::string name, float x, float y, float z, float w)
	{
		UniformVal u;
		u.name = std::move(name);
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
		u.type = UniformVal::Type::TEX;
		u.tex_id = tex_id;
		uniforms.push_back(std::move(u));
	}
	void set_uniform_i1(std::string name, int v)
	{
		UniformVal u;
		u.name = std::move(name);
		u.type = UniformVal::Type::I1;
		u.i1 = v;
		uniforms.push_back(std::move(u));
	}
};

// Abstract base class for all declarative programs.
class ProgramBase {
    public:
	virtual ~ProgramBase() = default;

	// --- Program definition (override these) ---

	// Program name for registry lookup
	virtual std::string_view name() const = 0;

	// GLSL sources (typically from builtin_shader_source)
	virtual std::string_view vert_source() const = 0;
	virtual std::string_view frag_source() const = 0;

	// Prepare draw state from runtime input.
	// Returns false to skip this draw (e.g., texture not loaded).
	// ctx provides access to textures, fonts, viewport, camera, etc.
	virtual bool prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     DrawState &out_state) = 0;

	// --- Provided by base class ---

	// Compile and link the GLSL program. Called once by registry.
	// Returns true on success.
	bool compile();

	// Draw using prepared state.
	// Must be called after compile() and prepare().
	void draw(const DrawState &state);

	// Access to underlying GL program
	GLuint gl_program_id() const { return program_.id(); }
	const Program &gl_program() const { return program_; }

    protected:
	ProgramBase() = default;

	// Helper: set built-in uniforms (view, camera) from ctx
	void set_builtin_uniforms(const RenderContext &ctx, DrawState &state)
	{
		state.set_uniform_f2("view", ctx.view_w * 0.5f,
				     -ctx.view_h * 0.5f);
		state.set_uniform_f4("camera", ctx.camera[0], ctx.camera[1],
				     ctx.camera[2], ctx.camera[3]);
	}

    private:
	Program program_; // compiled GLSL program

	// Shared streaming VAO/VBO/EBO (lazily initialized)
	static void ensure_streaming_buffers();
	static GLuint stream_vao();
	static GLuint stream_vbo();
	static GLuint stream_ebo();

	// Global last program bound (for caching)
	static GLuint &last_program_bound();
};

} // namespace declgl
