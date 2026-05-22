#include "renderer/programs/img_fade_program.h"

#include "resources/texture.h"

namespace declgl::programs
{

bool ImgFadeProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures &builtin_textures,
			     DrawState &out_state)
{
	const Texture *mask = texture_field(fields, ctx, "mask");
	if (!mask)
		return false;

	prepare_fullscreen_quad(out_state);
	set_uniform_f1(out_state, "t", number_field(fields, "t", 0.f));
	set_uniform_i1(out_state, "invert_mask",
		       int_field(fields, "invert_mask", 0));
	set_uniform_tex(out_state, "mask", mask->id());
	if (builtin_textures.t1)
		set_uniform_tex(out_state, "t1", builtin_textures.t1);
	if (builtin_textures.t2)
		set_uniform_tex(out_state, "t2", builtin_textures.t2);
	return true;
}

} // namespace declgl::programs

