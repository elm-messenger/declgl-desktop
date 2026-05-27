// Shared helpers for fullscreen effect/compositor declarative programs.

#include "renderer/programs/effect_common.h"

#include "resources/texture_registry.h"

namespace declgl
{
namespace programs
{

float number_field(const ProgramCallFields &fields, std::string_view key,
		   float fallback)
{
	using mlregl::transport::common::Value;
	const ProgramCallField *field = find_field(fields, key);
	if (field && field->has_val() &&
	    field->val().kind_case() == Value::kNumberValue) {
		return static_cast<float>(field->val().number_value());
	}
	return fallback;
}

int int_field(const ProgramCallFields &fields, std::string_view key,
	      int fallback)
{
	return static_cast<int>(
		number_field(fields, key, static_cast<float>(fallback)));
}

void vec4_field(const ProgramCallFields &fields, std::string_view key,
		float fallback_x, float fallback_y, float fallback_z,
		float fallback_w, float out[4])
{
	using mlregl::transport::common::Value;
	out[0] = fallback_x;
	out[1] = fallback_y;
	out[2] = fallback_z;
	out[3] = fallback_w;

	const ProgramCallField *field = find_field(fields, key);
	if (!field || !field->has_val() ||
	    field->val().kind_case() != Value::kNumberArrayValue) {
		return;
	}

	const auto &arr = field->val().number_array_value().values();
	if (arr.size() < 4)
		return;
	out[0] = static_cast<float>(arr[0]);
	out[1] = static_cast<float>(arr[1]);
	out[2] = static_cast<float>(arr[2]);
	out[3] = static_cast<float>(arr[3]);
}

const Texture *texture_field(const ProgramCallFields &fields,
			     const RenderContext &ctx, std::string_view key)
{
	using mlregl::transport::common::Value;
	const ProgramCallField *field = find_field(fields, key);
	if (!field || !field->has_val() ||
	    field->val().kind_case() != Value::kStringValue || !ctx.textures) {
		return nullptr;
	}
	return ctx.textures->get(field->val().string_value());
}

} // namespace programs
} // namespace declgl
