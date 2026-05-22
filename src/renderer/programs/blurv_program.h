#pragma once

#include "builtin_shaders.h"
#include "renderer/programs/effect_common.h"

namespace declgl::programs
{

class BlurVProgram : public FullscreenProgram {
    public:
	std::string_view name() const override { return "blurv"; }
	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("blurv", ShaderKind::FRAG);
	}
	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;
};

} // namespace declgl::programs

