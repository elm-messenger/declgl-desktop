// renderer/programs/circle_program.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

class CircleProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "circle"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("circle", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("circle", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     DrawState &out_state) override;

    private:
	// NDC quad [-1,1]^2 (matches JS circle convention)
	static constexpr float kQuadPos[8] = {
		-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

class RoundedRectProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "roundedRect"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("circle", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("roundedRect", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     DrawState &out_state) override;

    private:
	static constexpr float kQuadPos[8] = {
		-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace programs
} // namespace declgl
