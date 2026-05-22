// renderer/programs/triangle_program.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

class TriangleProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "triangle"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("triangle", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("triangle", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     DrawState &out_state) override;
};

} // namespace programs
} // namespace declgl
