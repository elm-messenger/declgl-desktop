// gpu/program_registry.h — name → Program lookup.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "gpu/program.h"

namespace declgl
{

class ProgramRegistry {
    public:
	// Compile + link + register under `name`. Returns true on success;
	// on failure the registry is unchanged and the program is not stored.
	bool register_program(std::string_view name, std::string_view vert_src,
			      std::string_view frag_src);

	// Convenience for built-ins: looks up the vendored GLSL by name and
	// forwards to register_program. Returns false if no such builtin
	// exists.
	bool register_builtin(std::string_view name);

	// Like [register_builtin] but lets the caller choose where the
	// vertex and fragment sources come from independently. This is the
	// mechanism we use to faithfully reproduce the JS backend's
	// cross-program shader pairings:
	//   - `roundedRect` pairs `circle`'s vert with `roundedRect`'s frag
	//   - `quad` and `poly` reuse the entire `triangle` pair
	// The resulting compiled program is registered under `name`.
	bool register_builtin_alias(std::string_view name,
				    std::string_view vert_source_name,
				    std::string_view frag_source_name);

	// Returns nullptr if not found.
	const Program *get(std::string_view name) const;

	void clear()
	{
		programs_.clear();
	}

    private:
	std::unordered_map<std::string, std::unique_ptr<Program> > programs_;
};

} // namespace declgl
