// gpu/program.cc

#include "gpu/program.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace declgl
{

namespace
{

// Compile a single shader stage. On failure, fills `log` and returns 0.
GLuint compile_stage(GLenum stage, std::string_view src, std::string &log)
{
	GLuint sh = glCreateShader(stage);
	if (!sh) {
		log = "glCreateShader returned 0";
		return 0;
	}
	const GLchar *p = src.data();
	const GLint len = static_cast<GLint>(src.size());
	glShaderSource(sh, 1, &p, &len);
	glCompileShader(sh);

	GLint ok = GL_FALSE;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint loglen = 0;
		glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &loglen);
		std::vector<char> buf(loglen > 0 ? loglen : 1);
		glGetShaderInfoLog(sh, static_cast<GLsizei>(buf.size()),
				   nullptr, buf.data());
		log.assign(buf.data());
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

void enumerate_uniforms(GLuint prog,
			std::unordered_map<std::string, GLint> &out)
{
	GLint count = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
	GLint max_len = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_len);
	std::vector<char> name_buf(max_len > 0 ? max_len : 1);
	for (GLint i = 0; i < count; ++i) {
		GLsizei written = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(prog, static_cast<GLuint>(i),
				   static_cast<GLsizei>(name_buf.size()),
				   &written, &size, &type, name_buf.data());
		std::string name(name_buf.data(), written);
		// Strip "[0]" array suffix.
		const auto bracket = name.find('[');
		if (bracket != std::string::npos)
			name.resize(bracket);
		const GLint loc = glGetUniformLocation(prog, name.c_str());
		if (loc >= 0)
			out.emplace(std::move(name), loc);
	}
}

void enumerate_attributes(GLuint prog,
			  std::unordered_map<std::string, GLint> &out)
{
	GLint count = 0;
	glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &count);
	GLint max_len = 0;
	glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &max_len);
	std::vector<char> name_buf(max_len > 0 ? max_len : 1);
	for (GLint i = 0; i < count; ++i) {
		GLsizei written = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveAttrib(prog, static_cast<GLuint>(i),
				  static_cast<GLsizei>(name_buf.size()),
				  &written, &size, &type, name_buf.data());
		std::string name(name_buf.data(), written);
		const GLint loc = glGetAttribLocation(prog, name.c_str());
		if (loc >= 0)
			out.emplace(std::move(name), loc);
	}
}

} // namespace

Program::~Program()
{
	destroy();
}

Program::Program(Program &&o) noexcept : name_(std::move(o.name_)),
					 program_(o.program_),
					 error_(std::move(o.error_)),
					 uniforms_(std::move(o.uniforms_)),
					 attributes_(std::move(o.attributes_))
{
	o.program_ = 0;
}

Program &Program::operator=(Program &&o) noexcept
{
	if (this != &o) {
		destroy();
		name_ = std::move(o.name_);
		program_ = o.program_;
		error_ = std::move(o.error_);
		uniforms_ = std::move(o.uniforms_);
		attributes_ = std::move(o.attributes_);
		o.program_ = 0;
	}
	return *this;
}

void Program::destroy()
{
	if (program_) {
		glDeleteProgram(program_);
		program_ = 0;
	}
	uniforms_.clear();
	attributes_.clear();
}

bool Program::build(std::string_view name, std::string_view vert_src,
		    std::string_view frag_src)
{
	destroy();
	name_ = std::string(name);

	std::string log;

	GLuint vs = compile_stage(GL_VERTEX_SHADER, vert_src, log);
	if (!vs) {
		error_ = "vertex shader: " + log;
		std::fprintf(stderr, "[declgl/program:%s] %s\n", name_.c_str(),
			     error_.c_str());
		return false;
	}
	GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag_src, log);
	if (!fs) {
		glDeleteShader(vs);
		error_ = "fragment shader: " + log;
		std::fprintf(stderr, "[declgl/program:%s] %s\n", name_.c_str(),
			     error_.c_str());
		return false;
	}

	program_ = glCreateProgram();
	glAttachShader(program_, vs);
	glAttachShader(program_, fs);
	glLinkProgram(program_);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = GL_FALSE;
	glGetProgramiv(program_, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLint loglen = 0;
		glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &loglen);
		std::vector<char> buf(loglen > 0 ? loglen : 1);
		glGetProgramInfoLog(program_, static_cast<GLsizei>(buf.size()),
				    nullptr, buf.data());
		error_ = std::string("link: ") + buf.data();
		std::fprintf(stderr, "[declgl/program:%s] %s\n", name_.c_str(),
			     error_.c_str());
		glDeleteProgram(program_);
		program_ = 0;
		return false;
	}

	enumerate_uniforms(program_, uniforms_);
	enumerate_attributes(program_, attributes_);
	error_.clear();
	std::printf("[declgl/program:%s] linked: %zu uniforms, %zu attribs\n",
		    name_.c_str(), uniforms_.size(), attributes_.size());
	return true;
}

GLint Program::uniform_location(std::string_view name) const
{
	auto it = uniforms_.find(std::string(name));
	return it == uniforms_.end() ? -1 : it->second;
}

GLint Program::attribute_location(std::string_view name) const
{
	auto it = attributes_.find(std::string(name));
	return it == attributes_.end() ? -1 : it->second;
}

} // namespace declgl
