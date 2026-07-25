// renderer/programs/textbox_program.h
#pragma once

#include <string>
#include <unordered_set>

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

// textbox program: MSDF text rendering with dynamic glyph quads
class TextboxProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "textbox";
	}

	std::string_view vert_source() const override
	{
		return builtin_shader_source("text", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("text", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    private:
	std::unordered_set<std::string> missing_font_warnings_;
};

} // namespace programs
} // namespace declgl
