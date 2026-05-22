// renderer/program_base.cc

#include "renderer/program_base.h"

#include <array>

#include "log/log.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl
{

const ProgramCallField *find_field(const ProgramCallFields &fields,
				   std::string_view key)
{
	for (const auto &f : fields) {
		if (f.key() == key)
			return f.has_val() ? &f : nullptr;
	}
	return nullptr;
}

bool ProgramBase::compile()
{
	return program_.build(name(), vert_source(), frag_source());
}

// --- Shared streaming buffers ---

namespace
{
struct StreamingBuffers {
	GLuint vao = 0;
	GLuint vbo = 0;
	GLuint ebo = 0;
	GLuint last_program = 0;
};
StreamingBuffers &streaming_buffers()
{
	static StreamingBuffers s;
	return s;
}
} // namespace

void ProgramBase::ensure_streaming_buffers()
{
	auto &s = streaming_buffers();
	if (s.vao != 0)
		return;
	glGenVertexArrays(1, &s.vao);
	glGenBuffers(1, &s.vbo);
	glGenBuffers(1, &s.ebo);
}

GLuint ProgramBase::stream_vao()
{
	return streaming_buffers().vao;
}
GLuint ProgramBase::stream_vbo()
{
	return streaming_buffers().vbo;
}
GLuint ProgramBase::stream_ebo()
{
	return streaming_buffers().ebo;
}

GLuint &ProgramBase::last_program_bound()
{
	return streaming_buffers().last_program;
}

void ProgramBase::draw(const DrawState &state)
{
	// Use program (with caching)
	const GLuint prog_id = program_.id();
	if (prog_id != last_program_bound()) {
		glUseProgram(prog_id);
		last_program_bound() = prog_id;
	}

	// Set uniforms
	int texture_unit = 0;
	for (const auto &u : state.uniforms) {
		const GLint loc = program_.uniform_location(u.name);
		if (loc < 0)
			continue;

		switch (u.type) {
		case DrawState::UniformVal::Type::F1:
			glUniform1f(loc, u.f1);
			break;
		case DrawState::UniformVal::Type::F2:
			glUniform2f(loc, u.f2[0], u.f2[1]);
			break;
		case DrawState::UniformVal::Type::F3:
			glUniform3f(loc, u.f3[0], u.f3[1], u.f3[2]);
			break;
		case DrawState::UniformVal::Type::F4:
			glUniform4f(loc, u.f4[0], u.f4[1], u.f4[2], u.f4[3]);
			break;
		case DrawState::UniformVal::Type::I1:
			glUniform1i(loc, u.i1);
			break;
		case DrawState::UniformVal::Type::TEX:
			glActiveTexture(GL_TEXTURE0 + texture_unit);
			glBindTexture(GL_TEXTURE_2D, u.tex_id);
			glUniform1i(loc, texture_unit);
			++texture_unit;
			break;
		}
	}

	// Bind shared streaming VAO
	ensure_streaming_buffers();
	glBindVertexArray(stream_vao());

	// Calculate total attribute data size
	GLsizeiptr total_size = 0;
	for (const auto &a : state.static_attribs) {
		total_size += static_cast<GLsizeiptr>(a.vertex_count) *
			      a.components * sizeof(float);
	}
	for (const auto &a : state.dyn_attribs) {
		total_size +=
			static_cast<GLsizeiptr>(a.data.size()) * sizeof(float);
	}

	// Orphan and upload attribute data
	glBindBuffer(GL_ARRAY_BUFFER, stream_vbo());
	glBufferData(GL_ARRAY_BUFFER, total_size, nullptr, GL_STREAM_DRAW);

	GLintptr offset = 0;
	std::vector<GLuint> enabled_attribs;

	// Static attributes (pointer to program-owned data)
	for (const auto &a : state.static_attribs) {
		const GLint loc = program_.attribute_location(a.name);
		if (loc < 0)
			continue;

		const GLsizeiptr byte_size =
			static_cast<GLsizeiptr>(a.vertex_count) * a.components *
			sizeof(float);
		glBufferSubData(GL_ARRAY_BUFFER, offset, byte_size, a.data);
		glEnableVertexAttribArray(static_cast<GLuint>(loc));
		glVertexAttribPointer(static_cast<GLuint>(loc), a.components,
				      GL_FLOAT, GL_FALSE, 0,
				      reinterpret_cast<const void *>(offset));
		enabled_attribs.push_back(static_cast<GLuint>(loc));
		offset += byte_size;
	}

	// Dynamic attributes (copied into DrawState)
	for (const auto &a : state.dyn_attribs) {
		const GLint loc = program_.attribute_location(a.name);
		if (loc < 0)
			continue;

		const GLsizeiptr byte_size =
			static_cast<GLsizeiptr>(a.data.size()) * sizeof(float);
		glBufferSubData(GL_ARRAY_BUFFER, offset, byte_size,
				a.data.data());
		glEnableVertexAttribArray(static_cast<GLuint>(loc));
		glVertexAttribPointer(static_cast<GLuint>(loc), a.components,
				      GL_FLOAT, GL_FALSE, 0,
				      reinterpret_cast<const void *>(offset));
		enabled_attribs.push_back(static_cast<GLuint>(loc));
		offset += byte_size;
	}

	// Draw
	if (state.indexed) {
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_ebo());
		const uint32_t *idx_data = state.static_indices ?
						   state.static_indices :
						   state.indices.data();
		const GLsizeiptr idx_size =
			static_cast<GLsizeiptr>(state.count) * sizeof(uint32_t);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_size, idx_data,
			     GL_STREAM_DRAW);
		glDrawElements(state.primitive, state.count, GL_UNSIGNED_INT,
			       nullptr);
	} else {
		// Find vertex count from first attribute
		GLsizei vert_count = state.count;
		if (vert_count == 0) {
			if (!state.static_attribs.empty()) {
				vert_count =
					state.static_attribs[0].vertex_count;
			} else if (!state.dyn_attribs.empty()) {
				vert_count = static_cast<GLsizei>(
					state.dyn_attribs[0].data.size() /
					state.dyn_attribs[0].components);
			}
		}
		glDrawArrays(state.primitive, 0, vert_count);
	}

	// Cleanup: disable vertex attribs
	for (GLuint loc : enabled_attribs) {
		glDisableVertexAttribArray(loc);
	}

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

} // namespace declgl
