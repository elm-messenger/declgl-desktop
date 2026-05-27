// renderer/programs/texture_program.h
#pragma once

#include "renderer/program_base.h"
#include "builtin_shaders.h"

namespace declgl
{

class Texture; // forward declaration

namespace programs
{

// texture program: position from 'pos', texc hardcoded
class TextureProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "texture";
	}

	std::string_view vert_source() const override
	{
		return builtin_shader_source("texture", ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("texture", ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

	// Helper: resolve texture from field (public for use by related programs)
	static const Texture *resolve_texture(const ProgramCallFields &fields,
					      const RenderContext &ctx);

    protected:
	// Default UV quad (Y-flipped corners)
	static constexpr float kDefaultTexc[8] = { 0.f, 1.f, 1.f, 1.f,
						   1.f, 0.f, 0.f, 0.f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// textureCropped program: position from 'pos', texc from 'texc' field
class TextureCroppedProgram : public TextureProgram {
    public:
	std::string_view name() const override
	{
		return "textureCropped";
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;
};

// centeredTexture program: no position attribute, uses posize uniform
class CenteredTextureProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "centeredTexture";
	}

	std::string_view vert_source() const override
	{
		return builtin_shader_source("textureCentered",
					     ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("textureCentered",
					     ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	static constexpr float kDefaultTexc[8] = { 0.f, 1.f, 1.f, 1.f,
						   1.f, 0.f, 0.f, 0.f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

// centeredCroppedTexture program: texc from 'texc' field (4 floats -> 8)
class CenteredCroppedTextureProgram : public ProgramBase {
    public:
	std::string_view name() const override
	{
		return "centeredCroppedTexture";
	}

	std::string_view vert_source() const override
	{
		return builtin_shader_source("textureCroppedCentered",
					     ShaderKind::VERT);
	}
	std::string_view frag_source() const override
	{
		return builtin_shader_source("textureCentered",
					     ShaderKind::FRAG);
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	// texc2: NDC-style corners for posize expansion
	static constexpr float kTexc2[8] = { -0.5f, 0.5f,  0.5f,  0.5f,
					     0.5f,  -0.5f, -0.5f, -0.5f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
};

} // namespace programs
} // namespace declgl
