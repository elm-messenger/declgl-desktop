// renderer/programs/triangle_program.cc

#include "renderer/programs/triangle_program.h"

namespace declgl
{
namespace programs
{

bool TriangleProgram::prepare(const ProgramCallFields &fields,
			      const RenderContext &ctx, DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: position array from 'pos' field
	const ProgramCallField *pos_f = find_field(fields, "pos");
	if (!pos_f || !pos_f->has_val() ||
	    pos_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &pos_arr = pos_f->val().number_array_value().values();
	if (pos_arr.size() < 6) { // at least 3 vertices (vec2)
		return false;
	}

	// Required: color from 'color' field
	const ProgramCallField *col_f = find_field(fields, "color");
	if (!col_f || !col_f->has_val() ||
	    col_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &col_arr = col_f->val().number_array_value().values();
	if (col_arr.size() < 4) {
		return false;
	}

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = false;
	out_state.count = static_cast<GLsizei>(pos_arr.size() / 2);

	// Dynamic attribute: position (copy from protobuf)
	out_state.add_dyn_attrib("position", 2, pos_arr.data(),
				static_cast<size_t>(pos_arr.size() / 2));

	// Uniform: color
	out_state.set_uniform_f4(
		"color", static_cast<float>(col_arr[0]),
		static_cast<float>(col_arr[1]), static_cast<float>(col_arr[2]),
		static_cast<float>(col_arr[3]));

	// Built-in uniforms: view, camera
	set_builtin_uniforms(ctx, out_state);

	return true;
}

} // namespace programs
} // namespace declgl
