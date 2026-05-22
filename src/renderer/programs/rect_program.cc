// renderer/programs/rect_program.cc

#include "renderer/programs/rect_program.h"

namespace declgl
{
namespace programs
{

bool RectProgram::prepare(const ProgramCallFields &fields,
			  const RenderContext &ctx, DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: posize (x, y, w, h)
	const ProgramCallField *posize_f = find_field(fields, "posize");
	if (!posize_f || !posize_f->has_val() ||
	    posize_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &posize_arr = posize_f->val().number_array_value().values();
	if (posize_arr.size() < 4) {
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

	// Static position (unit quad [0,1]^2)
	out_state.add_static_attrib("position", 2, kQuadPos, 4);

	// Static indices
	out_state.static_indices = kQuadIndices;

	// Uniform: posize
	out_state.set_uniform_f4(
		"posize", static_cast<float>(posize_arr[0]),
		static_cast<float>(posize_arr[1]),
		static_cast<float>(posize_arr[2]),
		static_cast<float>(posize_arr[3]));

	// Uniform: angle (optional, default 0)
	float angle = 0.f;
	const ProgramCallField *angle_f = find_field(fields, "angle");
	if (angle_f && angle_f->has_val() &&
	    angle_f->val().kind_case() == Value::kNumberValue) {
		angle = static_cast<float>(angle_f->val().number_value());
	}
	out_state.set_uniform_f1("angle", angle);

	// Uniform: color
	out_state.set_uniform_f4(
		"color", static_cast<float>(col_arr[0]),
		static_cast<float>(col_arr[1]), static_cast<float>(col_arr[2]),
		static_cast<float>(col_arr[3]));

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

} // namespace programs
} // namespace declgl
