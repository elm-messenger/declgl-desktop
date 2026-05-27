// renderer/program_base.cc

#include "renderer/program_base.h"

#include <algorithm>
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
	if (!program_.build(name(), vert_source(), frag_source()))
		return false;
	return after_compile();
}

// --- Program-local streaming/static buffers ---

namespace
{
struct GlobalRenderState {
	GLuint last_program = 0;
};
GlobalRenderState &global_render_state()
{
	static GlobalRenderState s;
	return s;
}
} // namespace

ProgramBase::~ProgramBase()
{
	for (auto &b : static_attrib_buffers_) {
		if (b.buffer)
			glDeleteBuffers(1, &b.buffer);
	}
	for (auto &b : static_index_buffers_) {
		if (b.buffer)
			glDeleteBuffers(1, &b.buffer);
	}
	if (dynamic_vbo_)
		glDeleteBuffers(1, &dynamic_vbo_);
	if (dynamic_ebo_)
		glDeleteBuffers(1, &dynamic_ebo_);
	if (vao_)
		glDeleteVertexArrays(1, &vao_);
}

void ProgramBase::ensure_program_buffers()
{
	if (vao_ != 0)
		return;
	glGenVertexArrays(1, &vao_);
	glGenBuffers(1, &dynamic_vbo_);
	glGenBuffers(1, &dynamic_ebo_);
}

GLuint ProgramBase::static_attrib_buffer(const DrawState::StaticAttrib &a)
{
	for (const auto &b : static_attrib_buffers_) {
		if (b.data == a.data && b.vertex_count == a.vertex_count &&
		    b.components == a.components) {
			return b.buffer;
		}
	}

	StaticAttribBuffer b;
	b.data = a.data;
	b.vertex_count = a.vertex_count;
	b.components = a.components;
	glGenBuffers(1, &b.buffer);
	glBindBuffer(GL_ARRAY_BUFFER, b.buffer);
	glBufferData(GL_ARRAY_BUFFER,
		     static_cast<GLsizeiptr>(a.vertex_count) * a.components *
			     sizeof(float),
		     a.data, GL_STATIC_DRAW);
	static_attrib_buffers_.push_back(b);
	return b.buffer;
}

GLuint ProgramBase::static_index_buffer(const uint32_t *data, GLsizei count)
{
	for (const auto &b : static_index_buffers_) {
		if (b.data == data && b.count == count)
			return b.buffer;
	}

	StaticIndexBuffer b;
	b.data = data;
	b.count = count;
	glGenBuffers(1, &b.buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
		     static_cast<GLsizeiptr>(count) * sizeof(uint32_t), data,
		     GL_STATIC_DRAW);
	static_index_buffers_.push_back(b);
	return b.buffer;
}

GLuint &ProgramBase::last_program_bound()
{
	return global_render_state().last_program;
}

GLint ProgramBase::cached_uniform_location(std::string_view name) const
{
	for (const auto &entry : uniform_location_cache_) {
		if (entry.name == name)
			return entry.loc;
	}
	const GLint loc = program_.uniform_location(name);
	uniform_location_cache_.push_back({ std::string(name), loc });
	return loc;
}

GLint ProgramBase::cached_attribute_location(std::string_view name) const
{
	for (const auto &entry : attribute_location_cache_) {
		if (entry.name == name)
			return entry.loc;
	}
	const GLint loc = program_.attribute_location(name);
	attribute_location_cache_.push_back({ std::string(name), loc });
	return loc;
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
		const GLint loc =
			u.loc >= 0	? u.loc :
			!u.name.empty() ? program_.uniform_location(u.name) :
					  -1;
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

	// Bind program-local VAO. Static buffers are cached per ProgramBase
	// instance; dynamic buffers are reused for all draws of this program.
	ensure_program_buffers();
	glBindVertexArray(vao_);

	// Calculate dynamic attribute data size. Static attributes live in
	// persistent GL_STATIC_DRAW buffers and are not re-uploaded per draw.
	GLsizeiptr total_size = 0;
	for (const auto &a : state.dyn_attribs) {
		total_size +=
			static_cast<GLsizeiptr>(a.data.size()) * sizeof(float);
	}

	// Orphan dynamic attribute storage only when dynamic data exists.
	glBindBuffer(GL_ARRAY_BUFFER, dynamic_vbo_);
	if (total_size > 0) {
		glBufferData(GL_ARRAY_BUFFER, total_size, nullptr,
			     GL_STREAM_DRAW);
	}

	GLintptr offset = 0;
	std::vector<GLuint> used_attribs;

	// Static attributes (cached in program-owned GPU buffers)
	for (const auto &a : state.static_attribs) {
		const GLint loc =
			a.loc >= 0	? a.loc :
			!a.name.empty() ? program_.attribute_location(a.name) :
					  -1;
		if (loc < 0)
			continue;

		glBindBuffer(GL_ARRAY_BUFFER, static_attrib_buffer(a));
		glEnableVertexAttribArray(static_cast<GLuint>(loc));
		glVertexAttribPointer(static_cast<GLuint>(loc), a.components,
				      GL_FLOAT, GL_FALSE, 0, nullptr);
		used_attribs.push_back(static_cast<GLuint>(loc));
	}

	// Dynamic attributes (copied into DrawState)
	glBindBuffer(GL_ARRAY_BUFFER, dynamic_vbo_);
	for (const auto &a : state.dyn_attribs) {
		const GLint loc =
			a.loc >= 0	? a.loc :
			!a.name.empty() ? program_.attribute_location(a.name) :
					  -1;
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
		used_attribs.push_back(static_cast<GLuint>(loc));
		offset += byte_size;
	}

	// Keep per-program VAO state persistent, but disable attributes that were
	// used by an earlier draw of this program and are absent now.
	for (GLuint loc : enabled_attribs_) {
		if (std::find(used_attribs.begin(), used_attribs.end(), loc) ==
		    used_attribs.end()) {
			glDisableVertexAttribArray(loc);
		}
	}
	enabled_attribs_ = std::move(used_attribs);

	// Draw
	if (state.indexed) {
		if (state.static_indices) {
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
				     static_index_buffer(state.static_indices,
							 state.count));
		} else {
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dynamic_ebo_);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER,
				     static_cast<GLsizeiptr>(state.count) *
					     sizeof(uint32_t),
				     state.indices.data(), GL_STREAM_DRAW);
		}
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
}

} // namespace declgl
