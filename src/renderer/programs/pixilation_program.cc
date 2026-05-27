#include "renderer/programs/pixilation_program.h"

namespace declgl::programs
{

bool PixilationProgram::prepare(const ProgramCallFields &fields,
				const RenderContext &ctx,
				const BuiltinTextures &builtin_textures,
				DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);
	set_uniform_f1(out_state, "pixelSize", number_field(fields, "ps", 1.f));
	if (builtin_textures.texture)
		set_uniform_tex(out_state, "tex", builtin_textures.texture);
	set_builtin_uniforms(ctx, out_state);
	return true;
}

} // namespace declgl::programs
