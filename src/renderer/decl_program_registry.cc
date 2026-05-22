// renderer/decl_program_registry.cc

#include "renderer/decl_program_registry.h"

#include "log/log.h"
#include "renderer/programs/triangle_program.h"
#include "renderer/programs/rect_program.h"
#include "renderer/programs/circle_program.h"
#include "renderer/programs/texture_program.h"
#include "renderer/programs/poly_program.h"
#include "renderer/programs/effect_programs.h"
#include "renderer/programs/textbox_program.h"

#include <algorithm>
#include <iterator>

namespace declgl
{

namespace
{

using BuiltinProgramFactory = std::unique_ptr<ProgramBase> (*)();

template <typename ProgramT>
std::unique_ptr<ProgramBase> make_builtin_program()
{
	return std::make_unique<ProgramT>();
}

struct BuiltinProgramSpec {
	std::string_view name;
	BuiltinProgramFactory factory;
};

// The single source of truth for desktop built-in declarative programs.
// To add a new built-in program, include its header above and add exactly one
// row here. Both selective registration (StartRegl.builtin_programs) and bulk
// registration use this inventory, so there is no second list to keep in sync.
static constexpr BuiltinProgramSpec kBuiltinPrograms[] = {
	// Primitives
	{ "triangle", &make_builtin_program<programs::TriangleProgram> },
	{ "rect", &make_builtin_program<programs::RectProgram> },
	{ "circle", &make_builtin_program<programs::CircleProgram> },
	{ "roundedRect", &make_builtin_program<programs::RoundedRectProgram> },
	{ "poly", &make_builtin_program<programs::PolyProgram> },
	{ "quad", &make_builtin_program<programs::QuadProgram> },

	// Textures
	{ "texture", &make_builtin_program<programs::TextureProgram> },
	{ "textureCropped",
	  &make_builtin_program<programs::TextureCroppedProgram> },
	{ "centeredTexture",
	  &make_builtin_program<programs::CenteredTextureProgram> },
	{ "centeredCroppedTexture",
	  &make_builtin_program<programs::CenteredCroppedTextureProgram> },

	// Effects / compositors / internal palette blit
	{ "palette", &make_builtin_program<programs::PaletteProgram> },
	{ "defaultCompositor",
	  &make_builtin_program<programs::DefaultCompositorProgram> },
	{ "compFade", &make_builtin_program<programs::CompFadeProgram> },
	{ "alphamult", &make_builtin_program<programs::AlphaMultProgram> },
	{ "colormult", &make_builtin_program<programs::ColorMultProgram> },
	{ "blurh", &make_builtin_program<programs::BlurHProgram> },
	{ "blurv", &make_builtin_program<programs::BlurVProgram> },
	{ "gblurh", &make_builtin_program<programs::GBlurHProgram> },
	{ "gblurv", &make_builtin_program<programs::GBlurVProgram> },
	{ "crt", &make_builtin_program<programs::CrtProgram> },
	{ "fxaa", &make_builtin_program<programs::FxaaProgram> },
	{ "outline", &make_builtin_program<programs::OutlineProgram> },
	{ "pixilation", &make_builtin_program<programs::PixilationProgram> },
	{ "imgFade", &make_builtin_program<programs::ImgFadeProgram> },

	// Textbox (MSDF text rendering)
	{ "textbox", &make_builtin_program<programs::TextboxProgram> },
};

const BuiltinProgramSpec *find_builtin_program(std::string_view name)
{
	auto it = std::find_if(std::begin(kBuiltinPrograms),
				       std::end(kBuiltinPrograms),
				       [name](const BuiltinProgramSpec &spec) {
					       return spec.name == name;
				       });
	return it == std::end(kBuiltinPrograms) ? nullptr : it;
}

} // namespace

bool register_builtin_decl_program(DeclProgramRegistry &registry,
				   std::string_view name)
{
	const BuiltinProgramSpec *spec = find_builtin_program(name);
	if (!spec)
		return false;

	std::unique_ptr<ProgramBase> program = spec->factory();
	if (program->name() != spec->name) {
		DECLGL_LOG_ERROR(
			"builtin program registry mismatch: inventory key '{}' "
			"constructs program '{}'",
			spec->name, program->name());
		return false;
	}

	registry.register_program(std::move(program));
	return true;
}

bool DeclProgramRegistry::compile_all()
{
	bool ok = true;
	for (auto &[name, prog] : programs_) {
		if (!prog->compile()) {
			DECLGL_LOG_ERROR("failed to compile program '{}'",
					 name);
			ok = false;
		}
	}
	return ok;
}

void register_builtin_decl_programs(DeclProgramRegistry &registry)
{
	for (const BuiltinProgramSpec &spec : kBuiltinPrograms) {
		(void)register_builtin_decl_program(registry, spec.name);
	}
}

} // namespace declgl
