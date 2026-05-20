// gpu/program.h — GLSL program compile/link + uniform/attribute lookup.

#pragma once

#include <glad/gl.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace declgl {

class Program {
public:
    Program() = default;
    ~Program();

    Program(const Program&)            = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&& o) noexcept;
    Program& operator=(Program&& o) noexcept;

    // Compiles the two stages, links into a program, and populates the
    // uniform/attribute lookup tables. Returns true on success; on
    // failure, [error()] holds the OpenGL compile/link log.
    bool build(std::string_view name,
               std::string_view vert_src,
               std::string_view frag_src);

    // -1 if not found (or inactive after linker dead-code-elimination).
    GLint uniform_location(std::string_view name) const;
    GLint attribute_location(std::string_view name) const;

    GLuint id() const { return program_; }
    const std::string& gl_name() const { return name_; }
    const std::string& error() const { return error_; }

private:
    void destroy();

    std::string name_;
    GLuint      program_ = 0;
    std::string error_;
    std::unordered_map<std::string, GLint> uniforms_;
    std::unordered_map<std::string, GLint> attributes_;
};

}  // namespace declgl
