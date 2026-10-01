#pragma once

#include <string>
#include <string_view>

namespace declgl::programs
{

enum class ShaderStage { Vertex, Fragment };

struct GlslEs100Identifiers {
	std::string texture;
	std::string texture_proj;
};

GlslEs100Identifiers
choose_glsl_es_100_identifiers(std::string_view vert_source,
			       std::string_view frag_source);

std::string
translate_glsl_es_100_identifier(std::string_view identifier,
				 const GlslEs100Identifiers &identifiers);

bool translate_glsl_es_100(std::string_view source, ShaderStage stage,
			   const GlslEs100Identifiers &identifiers,
			   std::string &output, std::string &error);

} // namespace declgl::programs
