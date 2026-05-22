// renderer/programs/effect_programs.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{
namespace programs
{

// palette program: fullscreen quad with texture sampling
class PaletteProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "palette"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("palette", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	// Fullscreen UV quad (JS convention: [1,1, 1,0, 0,0, 0,1])
	static constexpr float kFullscreenTexc[8] = {
		1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// defaultCompositor program: blend two textures
class DefaultCompositorProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "defaultCompositor"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("defaultCompositor", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	static constexpr float kFullscreenTexc[8] = {
		1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// compFade program: cross-fade between two textures
class CompFadeProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "compFade"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("compFade", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	static constexpr float kFullscreenTexc[8] = {
		1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// alphamult program: multiply alpha channel
class AlphaMultProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "alphamult"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("alphamult", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	static constexpr float kFullscreenTexc[8] = {
		1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// colormult program: multiply color channel
class ColorMultProgram : public ProgramBase {
    public:
	std::string_view name() const override { return "colormult"; }

	std::string_view vert_source() const override
	{
		return builtin_shader_source("effect", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("colormult", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	static constexpr float kFullscreenTexc[8] = {
		1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f
	};
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace programs
} // namespace declgl
