#pragma once

// resources/font_registry.h — name → Font map.
//
// Companion to [TextureRegistry]: every loaded MSDF font occupies a
// slot here keyed by its [LoadFont.name], and a parallel slot in the
// TextureRegistry keyed by [LoadFont.image_url] (or `name` when the
// image is shared). The walker resolves a textbox's `fonts` field to
// a Font* via this registry, and uses `Font::texture_name()` to look
// up the matching atlas texture in the TextureRegistry.

#include "resources/font.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace declgl {

// One registry entry. Owns the parsed Font, plus remembers the atlas
// texture key the walker should bind for this font.
struct FontEntry {
    std::unique_ptr<Font> font;
    std::string           texture_name;
};

class FontRegistry {
public:
    FontRegistry()  = default;
    ~FontRegistry() = default;

    FontRegistry(const FontRegistry&)            = delete;
    FontRegistry& operator=(const FontRegistry&) = delete;

    // Take ownership of [font] and register under [name], remembering
    // [texture_name] so the walker can look up the matching texture in
    // the TextureRegistry. Replaces an existing entry of the same
    // name (mirrors the JS backend's behaviour).
    void register_font(std::string_view name,
                       std::unique_ptr<Font> font,
                       std::string_view texture_name);

    // Returns nullptr if not registered. Pointer remains valid as long
    // as [name] is not unregistered or replaced.
    const FontEntry* get(std::string_view name) const;

    bool unregister_font(std::string_view name);

    std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::string, FontEntry> map_;
};

}  // namespace declgl
