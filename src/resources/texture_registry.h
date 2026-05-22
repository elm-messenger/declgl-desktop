#pragma once

// resources/texture_registry.h — name → Texture map.
//
// Engine-owned registry used by declarative programs to resolve texture
// field string values to GL texture handles for binding.

#include "resources/texture.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace declgl
{

class TextureRegistry {
    public:
	TextureRegistry() = default;
	~TextureRegistry() = default;

	TextureRegistry(const TextureRegistry &) = delete;
	TextureRegistry &operator=(const TextureRegistry &) = delete;

	// Take ownership of [tex] and register it under [name]. If a
	// texture with the same name already exists it is replaced
	// (mirrors the JS backend's behaviour: re-loading swaps the GL
	// object atomically).
	void register_texture(std::string_view name,
			      std::unique_ptr<Texture> tex);

	// Returns nullptr if not registered. The pointer remains valid as
	// long as [name] is not unregistered or replaced.
	const Texture *get(std::string_view name) const;

	// Drop the named texture (closes its GL handle). No-op if absent.
	bool unregister_texture(std::string_view name);

	// Useful for diagnostics.
	std::size_t size() const
	{
		return map_.size();
	}

    private:
	std::unordered_map<std::string, std::unique_ptr<Texture> > map_;
};

} // namespace declgl
