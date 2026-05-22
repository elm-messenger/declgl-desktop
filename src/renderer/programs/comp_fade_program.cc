#include "renderer/programs/comp_fade_program.h"

namespace declgl::programs
{

bool CompFadeProgram::prepare(const ProgramCallFields &fields,
			      const RenderContext &/*ctx*/,
			      const BuiltinTextures &builtin_textures,
			      DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);
	set_uniform_f1(out_state, "t", number_field(fields, "t", 0.f));
	if (builtin_textures.t1)
		set_uniform_tex(out_state, "t1", builtin_textures.t1);
	if (builtin_textures.t2)
		set_uniform_tex(out_state, "t2", builtin_textures.t2);
	return true;
}

} // namespace declgl::programs

