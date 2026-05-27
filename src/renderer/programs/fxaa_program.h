#pragma once

#include "builtin_shaders.h"
#include "renderer/program_base.h"

namespace declgl::programs
{

class FxaaProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "fxaa";
	}
	std::string_view vert_source() const override
	{
		return builtin_shader_source("fxaa", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("fxaa", ShaderKind::FRAG);
	}
	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    private:
	static constexpr float kFullscreenPosition[8] = { -1.f, 1.f, -1.f,
							  -1.f, 1.f, -1.f,
							  1.f,	1.f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace declgl::programs
