// renderer/programs/circle_program.cc

#include "renderer/programs/circle_program.h"

namespace declgl
{
namespace programs
{

bool CircleProgram::prepare(const ProgramCallFields &fields,
			    const RenderContext &ctx,
			    const BuiltinTextures &/*builtin_textures*/,
			    DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: cr (circle radius: x, y, r)
	const ProgramCallField *cr_f = find_field(fields, "cr");
	if (!cr_f || !cr_f->has_val() ||
	    cr_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &cr_arr = cr_f->val().number_array_value().values();
	if (cr_arr.size() < 3) {
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

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static NDC quad
	add_static_attrib(out_state, "position", 2, kQuadPos, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: cr (x, y, radius)
	set_uniform_f3(out_state, "cr", static_cast<float>(cr_arr[0]),
		       static_cast<float>(cr_arr[1]),
		       static_cast<float>(cr_arr[2]));

	// Uniform: color
	set_uniform_f4(
		out_state, "color", static_cast<float>(col_arr[0]),
		static_cast<float>(col_arr[1]), static_cast<float>(col_arr[2]),
		static_cast<float>(col_arr[3]));

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

bool RoundedRectProgram::prepare(const ProgramCallFields &fields,
				 const RenderContext &ctx,
				 const BuiltinTextures &/*builtin_textures*/,
				 DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Required: cs (corner spec: x, y, w, h)
	const ProgramCallField *cs_f = find_field(fields, "cs");
	if (!cs_f || !cs_f->has_val() ||
	    cs_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &cs_arr = cs_f->val().number_array_value().values();
	if (cs_arr.size() < 4) {
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

	// Required: radius
	float radius = 0.f;
	const ProgramCallField *r_f = find_field(fields, "radius");
	if (r_f && r_f->has_val() &&
	    r_f->val().kind_case() == Value::kNumberValue) {
		radius = static_cast<float>(r_f->val().number_value());
	}

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static NDC quad
	add_static_attrib(out_state, "position", 2, kQuadPos, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: cs (x, y, w, h)
	set_uniform_f4(
		out_state, "cs", static_cast<float>(cs_arr[0]),
		static_cast<float>(cs_arr[1]), static_cast<float>(cs_arr[2]),
		static_cast<float>(cs_arr[3]));

	// Uniform: radius
	set_uniform_f1(out_state, "radius", radius);

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
