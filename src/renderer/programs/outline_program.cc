#include "renderer/programs/outline_program.h"

namespace declgl::programs
{

bool OutlineProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures &builtin_textures,
			     DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);

	float color[4];
	vec4_field(fields, "color", 1.f, 1.f, 1.f, 1.f, color);
	set_uniform_f4(out_state, "color", color[0], color[1], color[2],
		       color[3]);
	set_uniform_f1(out_state, "outline",
		       number_field(fields, "outline", 0.f));
	if (builtin_textures.texture)
		set_uniform_tex(out_state, "tex", builtin_textures.texture);
	set_builtin_uniforms(ctx, out_state);
	return true;
}

} // namespace declgl::programs

