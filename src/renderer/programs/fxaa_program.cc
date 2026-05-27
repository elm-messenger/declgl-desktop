#include "renderer/programs/fxaa_program.h"

namespace declgl::programs
{

bool FxaaProgram::prepare(const ProgramCallFields & /*fields*/,
			  const RenderContext &ctx,
			  const BuiltinTextures &builtin_textures,
			  DrawState &out_state)
{
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = 6;
	add_static_attrib(out_state, "position", 2, kFullscreenPosition, 4);
	out_state.static_indices = kQuadIndices;

	if (builtin_textures.texture)
		set_uniform_tex(out_state, "tex", builtin_textures.texture);
	set_builtin_uniforms(ctx, out_state);
	return true;
}

} // namespace declgl::programs
