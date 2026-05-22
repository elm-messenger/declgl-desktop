// renderer/decl_program_registry.cc

#include "renderer/decl_program_registry.h"

#include "log/log.h"
#include "renderer/programs/triangle_program.h"
#include "renderer/programs/rect_program.h"
#include "renderer/programs/circle_program.h"
#include "renderer/programs/texture_program.h"
#include "renderer/programs/poly_program.h"
#include "renderer/programs/effect_programs.h"

namespace declgl
{

bool DeclProgramRegistry::compile_all()
{
	bool ok = true;
	for (auto &[name, prog] : programs_) {
		if (!prog->compile()) {
			DECLGL_LOG_ERROR("failed to compile program '{}'", name);
			ok = false;
		}
	}
	return ok;
}

void register_builtin_decl_programs(DeclProgramRegistry &registry)
{
	// Primitives
	registry.register_program(std::make_unique<programs::TriangleProgram>());
	registry.register_program(std::make_unique<programs::RectProgram>());
	registry.register_program(std::make_unique<programs::CircleProgram>());
	registry.register_program(
		std::make_unique<programs::RoundedRectProgram>());
	registry.register_program(std::make_unique<programs::PolyProgram>());
	registry.register_program(std::make_unique<programs::QuadProgram>());

	// Textures
	registry.register_program(std::make_unique<programs::TextureProgram>());
	registry.register_program(
		std::make_unique<programs::TextureCroppedProgram>());
	registry.register_program(
		std::make_unique<programs::CenteredTextureProgram>());
	registry.register_program(
		std::make_unique<programs::CenteredCroppedTextureProgram>());

	// Effects / compositors
	registry.register_program(std::make_unique<programs::PaletteProgram>());
	registry.register_program(
		std::make_unique<programs::DefaultCompositorProgram>());
	registry.register_program(std::make_unique<programs::CompFadeProgram>());
	registry.register_program(std::make_unique<programs::AlphaMultProgram>());
	registry.register_program(std::make_unique<programs::ColorMultProgram>());

	// Note: textbox is more complex and will be added later
}

} // namespace declgl
