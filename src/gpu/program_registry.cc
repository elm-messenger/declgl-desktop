// gpu/program_registry.cc

#include "gpu/program_registry.h"

#include <cstdio>

#include "builtin_shaders.h"

namespace declgl {

bool ProgramRegistry::register_program(std::string_view name,
                                       std::string_view vert_src,
                                       std::string_view frag_src) {
    auto p = std::make_unique<Program>();
    if (!p->build(name, vert_src, frag_src)) {
        return false;
    }
    programs_[std::string(name)] = std::move(p);
    return true;
}

bool ProgramRegistry::register_builtin(std::string_view name) {
    const auto vert = builtin_shader_source(name, ShaderKind::VERT);
    const auto frag = builtin_shader_source(name, ShaderKind::FRAG);
    if (vert.empty() || frag.empty()) {
        std::fprintf(stderr,
                     "[declgl/registry] builtin '%.*s' not vendored "
                     "(vert=%zu frag=%zu)\n",
                     static_cast<int>(name.size()), name.data(),
                     vert.size(), frag.size());
        return false;
    }
    return register_program(name, vert, frag);
}

const Program* ProgramRegistry::get(std::string_view name) const {
    auto it = programs_.find(std::string(name));
    return it == programs_.end() ? nullptr : it->second.get();
}

}  // namespace declgl
