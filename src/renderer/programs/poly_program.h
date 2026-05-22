// renderer/programs/poly_program.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

// poly program: generic polygon with custom primitive and optional indices
class PolyProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "poly"; }

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

// quad program: 4-vertex quad with indices
class QuadProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "quad"; }

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

    private:
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace programs
} // namespace declgl
