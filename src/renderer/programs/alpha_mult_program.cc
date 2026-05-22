#include "renderer/programs/alpha_mult_program.h"

namespace declgl::programs
{

bool AlphaMultProgram::prepare(const ProgramCallFields &fields,
			       const RenderContext &/*ctx*/,
			       const BuiltinTextures &builtin_textures,
			       DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);
	set_uniform_f1(out_state, "alpha", number_field(fields, "alpha", 1.f));
	if (builtin_textures.texture)
		set_uniform_tex(out_state, "tex", builtin_textures.texture);
	return true;
}

} // namespace declgl::programs

