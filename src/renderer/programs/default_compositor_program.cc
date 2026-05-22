#include "renderer/programs/default_compositor_program.h"

namespace declgl::programs
{

bool DefaultCompositorProgram::prepare(
	const ProgramCallFields &fields, const RenderContext &/*ctx*/,
	const BuiltinTextures &builtin_textures, DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);
	set_uniform_i1(out_state, "mode", int_field(fields, "mode", 0));
	if (builtin_textures.t1)
		set_uniform_tex(out_state, "t1", builtin_textures.t1);
	if (builtin_textures.t2)
		set_uniform_tex(out_state, "t2", builtin_textures.t2);
	return true;
}

} // namespace declgl::programs

