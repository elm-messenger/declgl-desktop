#include "resources/font_registry.h"

#include <utility>

namespace declgl {

void FontRegistry::register_font(std::string_view name,
                                 std::unique_ptr<Font> font,
                                 std::string_view texture_name) {
    FontEntry entry;
    entry.font         = std::move(font);
    entry.texture_name = std::string(texture_name);
    map_[std::string(name)] = std::move(entry);
}

const FontEntry* FontRegistry::get(std::string_view name) const {
    auto it = map_.find(std::string(name));
    return it == map_.end() ? nullptr : &it->second;
}

bool FontRegistry::unregister_font(std::string_view name) {
    return map_.erase(std::string(name)) > 0;
}

}  // namespace declgl
