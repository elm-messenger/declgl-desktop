// Shared helpers for fullscreen effect/compositor declarative programs.
#pragma once

#include "renderer/program_base.h"

namespace declgl
{
class Texture;

namespace programs
{

class FullscreenProgram : public ProgramBase {
    protected:
	static constexpr float kFullscreenTexc[8] = { 1.f, 1.f, 1.f, 0.f,
						      0.f, 0.f, 0.f, 1.f };
	static constexpr uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };

	void prepare_fullscreen_quad(DrawState &out_state) const
	{
		out_state.primitive = GL_TRIANGLES;
		out_state.indexed = true;
		out_state.count = 6;
		add_static_attrib(out_state, "texc", 2, kFullscreenTexc, 4);
		out_state.static_indices = kQuadIndices;
	}
};

float number_field(const ProgramCallFields &fields, std::string_view key,
		   float fallback);
int int_field(const ProgramCallFields &fields, std::string_view key,
	      int fallback);
void vec4_field(const ProgramCallFields &fields, std::string_view key,
		float fallback_x, float fallback_y, float fallback_z,
		float fallback_w, float out[4]);

const Texture *texture_field(const ProgramCallFields &fields,
			     const RenderContext &ctx, std::string_view key);

} // namespace programs
} // namespace declgl
