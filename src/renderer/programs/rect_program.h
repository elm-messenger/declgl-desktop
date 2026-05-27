// renderer/programs/rect_program.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

class RectProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "rect";
	}

	std::string_view vert_source() const override
	{
		return builtin_shader_source("rect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("rect", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    private:
	// Static position: [0,1]^2 quad (matches JS rect convention)
	static constexpr float kQuadPos[8] = { 0.f, 1.f, 1.f, 1.f,
					       1.f, 0.f, 0.f, 0.f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace programs
} // namespace declgl
