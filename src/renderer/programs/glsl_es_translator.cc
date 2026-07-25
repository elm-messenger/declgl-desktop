#include "renderer/programs/glsl_es_translator.h"

#include <cctype>
#include <string>

namespace declgl::programs
{
namespace
{

bool identifier_start(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool identifier_char(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string trim(std::string_view text)
{
	std::size_t first = 0;
	while (first < text.size() &&
	       std::isspace(static_cast<unsigned char>(text[first])))
		++first;
	std::size_t last = text.size();
	while (last > first &&
	       std::isspace(static_cast<unsigned char>(text[last - 1])))
		--last;
	return std::string(text.substr(first, last - first));
}

bool preprocess_directives(std::string_view source, std::string &output,
			   std::string &error)
{
	std::size_t offset = 0;
	std::size_t line_number = 1;
	while (offset < source.size()) {
		const std::size_t end = source.find('\n', offset);
		const std::size_t length =
			end == std::string_view::npos ? source.size() - offset :
						       end - offset;
		const std::string_view line = source.substr(offset, length);
		const std::string stripped = trim(line);
		bool omit = false;
		if (stripped.rfind("#version", 0) == 0) {
			if (stripped != "#version 100" &&
			    stripped != "#version 100 es") {
				error = "line " + std::to_string(line_number) +
					": expected GLSL ES 1.00 #version";
				return false;
			}
			omit = true;
		} else if (stripped.rfind("#extension", 0) == 0) {
			if (stripped.find("GL_OES_standard_derivatives") ==
			    std::string::npos) {
				error = "line " + std::to_string(line_number) +
					": unsupported GLSL ES extension directive '" +
					stripped + "'";
				return false;
			}
			omit = true;
		}
		if (!omit)
			output.append(line);
		if (end != std::string_view::npos) {
			output.push_back('\n');
			offset = end + 1;
			++line_number;
		} else {
			offset = source.size();
		}
	}
	return true;
}

} // namespace

bool translate_glsl_es_100(std::string_view source, ShaderStage stage,
			   std::string &output, std::string &error)
{
	std::string input;
	if (!preprocess_directives(source, input, error))
		return false;

	std::string body;
	body.reserve(input.size() + 64);
	std::size_t i = 0;
	std::size_t line = 1;
	bool uses_fragment_output = false;
	while (i < input.size()) {
		if (input[i] == '/' && i + 1 < input.size() &&
		    input[i + 1] == '/') {
			const std::size_t end = input.find('\n', i);
			if (end == std::string::npos) {
				body.append(input, i, input.size() - i);
				break;
			}
			body.append(input, i, end - i + 1);
			i = end + 1;
			++line;
			continue;
		}
		if (input[i] == '/' && i + 1 < input.size() &&
		    input[i + 1] == '*') {
			const std::size_t start = i;
			i += 2;
			while (i + 1 < input.size() &&
			       !(input[i] == '*' && input[i + 1] == '/')) {
				if (input[i] == '\n')
					++line;
				++i;
			}
			if (i + 1 >= input.size()) {
				error = "line " + std::to_string(line) +
					": unterminated block comment";
				return false;
			}
			i += 2;
			body.append(input, start, i - start);
			continue;
		}
		if (!identifier_start(input[i])) {
			body.push_back(input[i]);
			if (input[i] == '\n')
				++line;
			++i;
			continue;
		}

		const std::size_t start = i++;
		while (i < input.size() && identifier_char(input[i]))
			++i;
		const std::string token = input.substr(start, i - start);
		if (token == "precision") {
			while (i < input.size() && input[i] != ';') {
				if (input[i] == '\n') {
					body.push_back('\n');
					++line;
				}
				++i;
			}
			if (i == input.size()) {
				error = "line " + std::to_string(line) +
					": unterminated precision declaration";
				return false;
			}
			++i;
			continue;
		}
		if (token == "lowp" || token == "mediump" || token == "highp")
			continue;
		if (token == "attribute") {
			if (stage != ShaderStage::Vertex) {
				error = "line " + std::to_string(line) +
					": attribute is only valid in a vertex shader";
				return false;
			}
			body.append("in");
		} else if (token == "varying") {
			body.append(stage == ShaderStage::Vertex ? "out" : "in");
		} else if (token == "texture2D" || token == "texture2DProj" ||
			   token == "textureCube") {
			body.append(token == "texture2DProj" ? "textureProj" :
							       "texture");
		} else if (token == "gl_FragColor") {
			if (stage != ShaderStage::Fragment) {
				error = "line " + std::to_string(line) +
					": gl_FragColor is only valid in a fragment shader";
				return false;
			}
			uses_fragment_output = true;
			body.append("declgl_FragColor");
		} else if (token == "gl_FragData") {
			error = "line " + std::to_string(line) +
				": gl_FragData is not supported";
			return false;
		} else if (token == "gl_FragDepthEXT") {
			body.append("gl_FragDepth");
		} else {
			body.append(token);
		}
	}

	output = "#version 330 core\n";
	if (stage == ShaderStage::Fragment && uses_fragment_output)
		output += "out vec4 declgl_FragColor;\n";
	output += "#line 1\n";
	output += body;
	return true;
}

} // namespace declgl::programs
