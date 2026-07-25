#pragma once

#include <string>
#include <string_view>

namespace declgl::programs
{

enum class ShaderStage { Vertex, Fragment };

bool translate_glsl_es_100(std::string_view source, ShaderStage stage,
			   std::string &output, std::string &error);

} // namespace declgl::programs
