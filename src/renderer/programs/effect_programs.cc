// renderer/programs/effect_programs.cc

#include "renderer/programs/effect_programs.h"

#include "gpu/fbo_pool.h"

namespace declgl
{
namespace programs
{

bool PaletteProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const std::unordered_map<std::string, GLuint> &builtin_textures,
			     DrawState &out_state)
{
	// palette program is used internally for FBO blitting
	// It samples a texture and draws fullscreen quad

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc (fullscreen UVs)
	out_state.add_static_attrib("texc", 2, kFullscreenTexc, 4);

	// Static: indices
	out_state.static_indices = kQuadIndices;

	// Builtin texture: look for "tex", "fbo", or "texture"
	auto it = builtin_textures.find("tex");
	if (it == builtin_textures.end())
		it = builtin_textures.find("fbo");
	if (it == builtin_textures.end())
		it = builtin_textures.find("texture");
	if (it != builtin_textures.end()) {
		out_state.set_uniform_tex("tex", it->second);
	}

	return true;
}

bool DefaultCompositorProgram::prepare(
	const ProgramCallFields &fields,
	const RenderContext &ctx,
	const std::unordered_map<std::string, GLuint> &builtin_textures,
	DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc
	out_state.add_static_attrib("texc", 2, kFullscreenTexc, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: mode (blend mode)
	const ProgramCallField *mode_f = find_field(fields, "mode");
	if (mode_f && mode_f->has_val() &&
	    mode_f->val().kind_case() == Value::kNumberValue) {
		out_state.set_uniform_i1(
			"mode", static_cast<int>(mode_f->val().number_value()));
	} else {
		out_state.set_uniform_i1("mode", 0);
	}

	// Builtin textures: t1 and t2
	auto it1 = builtin_textures.find("t1");
	if (it1 != builtin_textures.end()) {
		out_state.set_uniform_tex("t1", it1->second);
	}
	auto it2 = builtin_textures.find("t2");
	if (it2 != builtin_textures.end()) {
		out_state.set_uniform_tex("t2", it2->second);
	}

	return true;
}

bool CompFadeProgram::prepare(
	const ProgramCallFields &fields,
	const RenderContext &ctx,
	const std::unordered_map<std::string, GLuint> &builtin_textures,
	DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc
	out_state.add_static_attrib("texc", 2, kFullscreenTexc, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: t (fade factor, 0..1)
	float t = 0.f;
	const ProgramCallField *t_f = find_field(fields, "t");
	if (t_f && t_f->has_val() &&
	    t_f->val().kind_case() == Value::kNumberValue) {
		t = static_cast<float>(t_f->val().number_value());
	}
	out_state.set_uniform_f1("t", t);

	// Builtin textures: t1 and t2
	auto it1 = builtin_textures.find("t1");
	if (it1 != builtin_textures.end()) {
		out_state.set_uniform_tex("t1", it1->second);
	}
	auto it2 = builtin_textures.find("t2");
	if (it2 != builtin_textures.end()) {
		out_state.set_uniform_tex("t2", it2->second);
	}

	return true;
}

bool AlphaMultProgram::prepare(
	const ProgramCallFields &fields,
	const RenderContext &ctx,
	const std::unordered_map<std::string, GLuint> &builtin_textures,
	DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc
	out_state.add_static_attrib("texc", 2, kFullscreenTexc, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: alpha
	float alpha = 1.f;
	const ProgramCallField *alpha_f = find_field(fields, "alpha");
	if (alpha_f && alpha_f->has_val() &&
	    alpha_f->val().kind_case() == Value::kNumberValue) {
		alpha = static_cast<float>(alpha_f->val().number_value());
	}
	out_state.set_uniform_f1("alpha", alpha);

	// Builtin texture
	auto it = builtin_textures.find("texture");
	if (it != builtin_textures.end()) {
		out_state.set_uniform_tex("texture", it->second);
	}

	return true;
}

bool ColorMultProgram::prepare(
	const ProgramCallFields &fields,
	const RenderContext &ctx,
	const std::unordered_map<std::string, GLuint> &builtin_textures,
	DrawState &out_state)
{
	using mlregl::transport::common::Value;

	// Setup draw state
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;

	// Static: texc
	out_state.add_static_attrib("texc", 2, kFullscreenTexc, 4);
	out_state.static_indices = kQuadIndices;

	// Uniform: color
	const ProgramCallField *col_f = find_field(fields, "color");
	if (col_f && col_f->has_val() &&
	    col_f->val().kind_case() == Value::kNumberArrayValue) {
		const auto &arr = col_f->val().number_array_value().values();
		if (arr.size() >= 4) {
			out_state.set_uniform_f4(
				"color", static_cast<float>(arr[0]),
				static_cast<float>(arr[1]),
				static_cast<float>(arr[2]),
				static_cast<float>(arr[3]));
		} else {
			out_state.set_uniform_f4("color", 1, 1, 1, 1);
		}
	} else {
		out_state.set_uniform_f4("color", 1, 1, 1, 1);
	}

	// Builtin texture
	auto it = builtin_textures.find("texture");
	if (it != builtin_textures.end()) {
		out_state.set_uniform_tex("texture", it->second);
	}

	return true;
}

} // namespace programs
} // namespace declgl
