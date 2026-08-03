#include <cstdlib>
#include <iostream>
#include <string>

#include "renderer/programs/glsl_es_translator.h"

namespace
{

void require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

} // namespace

int main()
{
	using declgl::programs::ShaderStage;
	using declgl::programs::choose_glsl_es_100_identifiers;
	using declgl::programs::translate_glsl_es_100;
	using declgl::programs::translate_glsl_es_100_identifier;

	const std::string vert =
		"attribute vec2 uv; varying vec2 vuv; void main() { vuv = uv; }";
	const std::string frag =
		"precision mediump float;\n"
		"uniform sampler2D texture;\n"
		"varying vec2 vuv;\n"
		"void main() { gl_FragColor = texture2D(texture, vuv); }\n";
	const auto identifiers = choose_glsl_es_100_identifiers(vert, frag);

	std::string translated;
	std::string error;
	require(translate_glsl_es_100(frag, ShaderStage::Fragment, identifiers,
				      translated, error),
		"fragment translation failed");
	require(translated.find("uniform sampler2D " + identifiers.texture) !=
			std::string::npos,
		"texture uniform was not renamed");
	require(translated.find("texture(" + identifiers.texture + ", vuv)") !=
			std::string::npos,
		"texture2D call or argument was translated incorrectly");
	require(translate_glsl_es_100_identifier("texture", identifiers) ==
			identifiers.texture,
		"uniform mapping did not follow the source rename");

	const auto collision = choose_glsl_es_100_identifiers(
		"float declgl_es_texture;", frag);
	require(collision.texture != "declgl_es_texture",
		"generated identifier collided with user source");

	return 0;
}
