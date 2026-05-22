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

namespace declgl
{

bool register_builtin_decl_program(DeclProgramRegistry &registry,
				   std::string_view name)
{
	// Primitives
	if (name == "triangle") {
		registry.register_program(
			std::make_unique<programs::TriangleProgram>());
		return true;
	}
	if (name == "rect") {
		registry.register_program(std::make_unique<programs::RectProgram>());
		return true;
	}
	if (name == "circle") {
		registry.register_program(
			std::make_unique<programs::CircleProgram>());
		return true;
	}
	if (name == "roundedRect") {
		registry.register_program(
			std::make_unique<programs::RoundedRectProgram>());
		return true;
	}
	if (name == "poly") {
		registry.register_program(std::make_unique<programs::PolyProgram>());
		return true;
	}
	if (name == "quad") {
		registry.register_program(std::make_unique<programs::QuadProgram>());
		return true;
	}

	// Textures
	if (name == "texture") {
		registry.register_program(
			std::make_unique<programs::TextureProgram>());
		return true;
	}
	if (name == "textureCropped") {
		registry.register_program(
			std::make_unique<programs::TextureCroppedProgram>());
		return true;
	}
	if (name == "centeredTexture") {
		registry.register_program(
			std::make_unique<programs::CenteredTextureProgram>());
		return true;
	}
	if (name == "centeredCroppedTexture") {
		registry.register_program(
			std::make_unique<programs::CenteredCroppedTextureProgram>());
		return true;
	}

	// Effects / compositors / internal palette blit
	if (name == "palette") {
		registry.register_program(
			std::make_unique<programs::PaletteProgram>());
		return true;
	}
	if (name == "defaultCompositor") {
		registry.register_program(
			std::make_unique<programs::DefaultCompositorProgram>());
		return true;
	}
	if (name == "compFade") {
		registry.register_program(
			std::make_unique<programs::CompFadeProgram>());
		return true;
	}
	if (name == "alphamult") {
		registry.register_program(
			std::make_unique<programs::AlphaMultProgram>());
		return true;
	}
	if (name == "colormult") {
		registry.register_program(
			std::make_unique<programs::ColorMultProgram>());
		return true;
	}

	// Textbox (MSDF text rendering)
	if (name == "textbox") {
		registry.register_program(
			std::make_unique<programs::TextboxProgram>());
		return true;
	}

	return false;
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
	static constexpr std::string_view names[] = {
		"triangle", "rect", "circle", "roundedRect", "poly", "quad",
		"texture", "textureCropped", "centeredTexture",
		"centeredCroppedTexture", "palette", "defaultCompositor",
		"compFade", "alphamult", "colormult", "textbox",
	};
	for (std::string_view name : names) {
		(void)register_builtin_decl_program(registry, name);
	}
}

} // namespace declgl
