#include "resources/texture_registry.h"

#include <utility>

namespace declgl {

void TextureRegistry::register_texture(std::string_view name,
                                       std::unique_ptr<Texture> tex) {
    map_[std::string(name)] = std::move(tex);
}

const Texture* TextureRegistry::get(std::string_view name) const {
    auto it = map_.find(std::string(name));
    return it == map_.end() ? nullptr : it->second.get();
}

bool TextureRegistry::unregister_texture(std::string_view name) {
    return map_.erase(std::string(name)) > 0;
}

}  // namespace declgl
