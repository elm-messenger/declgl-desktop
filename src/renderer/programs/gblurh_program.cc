#include "renderer/programs/gblurh_program.h"

namespace declgl::programs
{

bool GBlurHProgram::prepare(const ProgramCallFields &fields,
			    const RenderContext &ctx,
			    const BuiltinTextures &builtin_textures,
			    DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);
	set_uniform_f2(out_state, "dir", 1.f, 0.f);
	set_uniform_f1(out_state, "radius",
		       number_field(fields, "radius", 0.f));
	if (builtin_textures.texture)
		set_uniform_tex(out_state, "tex", builtin_textures.texture);
	set_builtin_uniforms(ctx, out_state);
	return true;
}

} // namespace declgl::programs
