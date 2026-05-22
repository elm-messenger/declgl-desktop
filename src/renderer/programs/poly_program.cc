// renderer/programs/poly_program.cc

#include "renderer/programs/poly_program.h"

namespace declgl
{
namespace programs
{

namespace {

GLenum primitive_from_string(std::string_view s)
{
	if (s == "points")
		return GL_POINTS;
	if (s == "lines")
		return GL_LINES;
	if (s == "line strip")
		return GL_LINE_STRIP;
	if (s == "line loop")
		return GL_LINE_LOOP;
	if (s == "triangles")
		return GL_TRIANGLES;
	if (s == "triangle strip")
		return GL_TRIANGLE_STRIP;
	if (s == "triangle fan")
		return GL_TRIANGLE_FAN;
	return GL_TRIANGLES;
}

} // namespace

bool PolyProgram::prepare(const ProgramCallFields &fields,
			  const RenderContext &ctx,
			  const BuiltinTextures &/*builtin_textures*/,
			  DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: position from 'pos' field
	const ProgramCallField *pos_f = find_field(fields, "pos");
	if (!pos_f || !pos_f->has_val() ||
	    pos_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &pos_arr = pos_f->val().number_array_value().values();
	if (pos_arr.size() < 2) {
		return false;
	}

	// Required: color
	const ProgramCallField *col_f = find_field(fields, "color");
	if (!col_f || !col_f->has_val() ||
	    col_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &col_arr = col_f->val().number_array_value().values();
	if (col_arr.size() < 4) {
		return false;
	}

	// Optional: primitive type (default: triangles)
	GLenum prim = GL_TRIANGLES;
	const ProgramCallField *prim_f = find_field(fields, "prim");
	if (prim_f && prim_f->has_val() &&
	    prim_f->val().kind_case() == Value::kStringValue) {
		prim = primitive_from_string(prim_f->val().string_value());
	}

	// Optional: indices from 'elem' field
	const ProgramCallField *elem_f = find_field(fields, "elem");
	bool has_indices = elem_f && elem_f->has_val() &&
			   elem_f->val().kind_case() == Value::kNumberArrayValue;

	// Setup draw state
	out_state.primitive = prim;

	// Dynamic: position
	add_dyn_attrib(out_state, "position", 2, pos_arr.data(),
		       static_cast<size_t>(pos_arr.size() / 2));

	if (has_indices) {
		// Indexed draw
		const auto &elem_arr = elem_f->val().number_array_value().values();
		out_state.indexed = true;
		out_state.count = static_cast<GLsizei>(elem_arr.size());
		out_state.indices.reserve(elem_arr.size());
		for (double d : elem_arr) {
			out_state.indices.push_back(static_cast<uint32_t>(d));
		}
	} else {
		// Array draw
		out_state.indexed = false;
		out_state.count = static_cast<GLsizei>(pos_arr.size() / 2);
	}

	// Uniform: color
	set_uniform_f4(
		out_state, "color", static_cast<float>(col_arr[0]),
		static_cast<float>(col_arr[1]), static_cast<float>(col_arr[2]),
		static_cast<float>(col_arr[3]));

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

bool QuadProgram::prepare(const ProgramCallFields &fields,
			  const RenderContext &ctx,
			  const BuiltinTextures &/*builtin_textures*/,
			  DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: position from 'pos' field (4 vertices = 8 floats)
	const ProgramCallField *pos_f = find_field(fields, "pos");
	if (!pos_f || !pos_f->has_val() ||
	    pos_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &pos_arr = pos_f->val().number_array_value().values();
	if (pos_arr.size() < 8) {
		return false;
	}

	// Required: color
	const ProgramCallField *col_f = find_field(fields, "color");
	if (!col_f || !col_f->has_val() ||
	    col_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &col_arr = col_f->val().number_array_value().values();
	if (col_arr.size() < 4) {
		return false;
	}

	// Setup draw state: indexed quad
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Dynamic: position
	add_dyn_attrib(out_state, "position", 2, pos_arr.data(), 4);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// Uniform: color
	set_uniform_f4(
		out_state, "color", static_cast<float>(col_arr[0]),
		static_cast<float>(col_arr[1]), static_cast<float>(col_arr[2]),
		static_cast<float>(col_arr[3]));

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

} // namespace programs
} // namespace declgl
