// renderer/programs/texture_program.cc

#include "renderer/programs/texture_program.h"

#include <array>

#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl
{
namespace programs
{

const Texture *TextureProgram::resolve_texture(const ProgramCallFields &fields,
					       const RenderContext &ctx)
{
	using mlregl::transport::common::Value;

	const ProgramCallField *tex_f = find_field(fields, "texture");
	if (!tex_f || !tex_f->has_val() ||
	    tex_f->val().kind_case() != Value::kStringValue) {
		return nullptr;
	}
	if (!ctx.textures) {
		return nullptr;
	}
	return ctx.textures->get(tex_f->val().string_value());
}

bool TextureProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures & /*builtin_textures*/,
			     DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Resolve texture
	const Texture *tex = resolve_texture(fields, ctx);
	if (!tex) {
		return false; // not loaded yet - skip draw
	}

	// Required: position from 'pos' field
	const ProgramCallField *pos_f = find_field(fields, "pos");
	if (!pos_f || !pos_f->has_val() ||
	    pos_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &pos_arr = pos_f->val().number_array_value().values();
	if (pos_arr.size() < 8) { // at least 4 vertices (quad)
		return false;
	}

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Dynamic: position
	add_dyn_attrib(out_state, "position", 2, pos_arr.data(),
		       static_cast<size_t>(pos_arr.size() / 2));

	// Static: texc (default UV quad)
	add_static_attrib(out_state, "texc", 2, kDefaultTexc, 4);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// GLSL sampler is named `tex` (not `texture`) to avoid shadowing the
	// GLSL texture() function.
	set_uniform_tex(out_state, "tex", tex->id());

	// Uniform: alpha (default 1.0)
	float alpha = 1.0f;
	const ProgramCallField *alpha_f = find_field(fields, "alpha");
	if (alpha_f && alpha_f->has_val() &&
	    alpha_f->val().kind_case() == Value::kNumberValue) {
		alpha = static_cast<float>(alpha_f->val().number_value());
	}
	set_uniform_f1(out_state, "alpha", alpha);

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

bool TextureCroppedProgram::prepare(
	const ProgramCallFields &fields, const RenderContext &ctx,
	const BuiltinTextures & /*builtin_textures*/, DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Resolve texture
	const Texture *tex = resolve_texture(fields, ctx);
	if (!tex) {
		return false;
	}

	// Required: position from 'pos' field
	const ProgramCallField *pos_f = find_field(fields, "pos");
	if (!pos_f || !pos_f->has_val() ||
	    pos_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &pos_arr = pos_f->val().number_array_value().values();
	if (pos_arr.size() < 8) {
		return false;
	}

	// Required: texc from 'texc' field
	const ProgramCallField *texc_f = find_field(fields, "texc");
	if (!texc_f || !texc_f->has_val() ||
	    texc_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &texc_arr = texc_f->val().number_array_value().values();
	if (texc_arr.size() < 8) {
		return false;
	}

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Dynamic: position
	add_dyn_attrib(out_state, "position", 2, pos_arr.data(),
		       static_cast<size_t>(pos_arr.size() / 2));

	// Dynamic: texc (from input)
	std::vector<float> texc_floats;
	texc_floats.reserve(texc_arr.size());
	for (double d : texc_arr) {
		texc_floats.push_back(static_cast<float>(d));
	}
	add_dyn_attrib(out_state, "texc", 2, texc_floats.data(),
		       texc_floats.size() / 2);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// Uniform: texture
	set_uniform_tex(out_state, "tex", tex->id());

	// Uniform: alpha (default 1.0)
	float alpha = 1.0f;
	const ProgramCallField *alpha_f = find_field(fields, "alpha");
	if (alpha_f && alpha_f->has_val() &&
	    alpha_f->val().kind_case() == Value::kNumberValue) {
		alpha = static_cast<float>(alpha_f->val().number_value());
	}
	set_uniform_f1(out_state, "alpha", alpha);

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

bool CenteredTextureProgram::prepare(
	const ProgramCallFields &fields, const RenderContext &ctx,
	const BuiltinTextures & /*builtin_textures*/, DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Resolve texture
	const Texture *tex = TextureProgram::resolve_texture(fields, ctx);
	if (!tex) {
		return false;
	}

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

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc (default UV quad) - no position attribute
	add_static_attrib(out_state, "texc", 2, kDefaultTexc, 4);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// Uniform: texture
	set_uniform_tex(out_state, "tex", tex->id());

	// Uniform: posize
	set_uniform_f4(out_state, "posize", static_cast<float>(posize_arr[0]),
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
	set_uniform_f1(out_state, "angle", angle);

	// Uniform: alpha (default 1.0)
	float alpha = 1.0f;
	const ProgramCallField *alpha_f = find_field(fields, "alpha");
	if (alpha_f && alpha_f->has_val() &&
	    alpha_f->val().kind_case() == Value::kNumberValue) {
		alpha = static_cast<float>(alpha_f->val().number_value());
	}
	set_uniform_f1(out_state, "alpha", alpha);

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

bool CenteredCroppedTextureProgram::prepare(
	const ProgramCallFields &fields, const RenderContext &ctx,
	const BuiltinTextures & /*builtin_textures*/, DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Resolve texture
	const Texture *tex = TextureProgram::resolve_texture(fields, ctx);
	if (!tex) {
		return false;
	}

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

	// Required: texc (cx, cy, cw, ch) -> expand to 8-float UV corners
	const ProgramCallField *texc_f = find_field(fields, "texc");
	if (!texc_f || !texc_f->has_val() ||
	    texc_f->val().kind_case() != Value::kNumberArrayValue) {
		return false;
	}
	const auto &texc_arr = texc_f->val().number_array_value().values();
	if (texc_arr.size() < 4) {
		return false;
	}

	// Expand 4-float (cx,cy,cw,ch) to 8-float per-corner UVs
	const float x1 = static_cast<float>(texc_arr[0]);
	const float y1 = static_cast<float>(texc_arr[1]);
	const float w = static_cast<float>(texc_arr[2]);
	const float h = static_cast<float>(texc_arr[3]);
	std::array<float, 8> expanded_texc = { x1,     y1,     x1 + w, y1,
					       x1 + w, y1 + h, x1,     y1 + h };

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Dynamic: texc (expanded)
	add_dyn_attrib(out_state, "texc", 2, expanded_texc.data(), 4);

	// Static: texc2 (NDC corners for posize expansion)
	add_static_attrib(out_state, "texc2", 2, kTexc2, 4);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// Uniform: texture
	set_uniform_tex(out_state, "tex", tex->id());

	// Uniform: posize
	set_uniform_f4(out_state, "posize", static_cast<float>(posize_arr[0]),
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
	set_uniform_f1(out_state, "angle", angle);

	// Uniform: alpha (default 1.0)
	float alpha = 1.0f;
	const ProgramCallField *alpha_f = find_field(fields, "alpha");
	if (alpha_f && alpha_f->has_val() &&
	    alpha_f->val().kind_case() == Value::kNumberValue) {
		alpha = static_cast<float>(alpha_f->val().number_value());
	}
	set_uniform_f1(out_state, "alpha", alpha);

	// Built-in uniforms
	set_builtin_uniforms(ctx, out_state);

	return true;
}

} // namespace programs
} // namespace declgl
