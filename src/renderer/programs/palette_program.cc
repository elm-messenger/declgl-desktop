#include "renderer/programs/palette_program.h"

namespace declgl::programs
{

bool PaletteProgram::prepare(const ProgramCallFields &/*fields*/,
			     const RenderContext &/*ctx*/,
			     const BuiltinTextures &builtin_textures,
			     DrawState &out_state)
{
	prepare_fullscreen_quad(out_state);

	const GLuint tex = builtin_textures.tex ? builtin_textures.tex :
			   builtin_textures.fbo ? builtin_textures.fbo :
						 builtin_textures.texture;
	if (tex)
		set_uniform_tex(out_state, "tex", tex);
	return true;
}

} // namespace declgl::programs

