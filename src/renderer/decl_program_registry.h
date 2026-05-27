// renderer/decl_program_registry.h — registry for declarative programs.
//
// Stores ProgramBase instances (one per program type).
// Each instance is created once and reused for all draws.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "renderer/program_base.h"

namespace declgl
{

class DeclProgramRegistry {
    public:
	DeclProgramRegistry() = default;

	// Register a program instance (takes ownership)
	void register_program(std::unique_ptr<ProgramBase> prog)
	{
		std::string name(prog->name());
		programs_[name] = std::move(prog);
	}

	// Get program by name (returns nullptr if not found)
	ProgramBase *get(std::string_view name) const
	{
		auto it = programs_.find(std::string(name));
		return it == programs_.end() ? nullptr : it->second.get();
	}

	// Compile all registered programs. Returns false if any failed.
	bool compile_all();

	void clear()
	{
		programs_.clear();
	}

    private:
	std::unordered_map<std::string, std::unique_ptr<ProgramBase> > programs_;
};

// Register all built-in declarative programs
void register_builtin_decl_programs(DeclProgramRegistry &registry);

// Register one built-in declarative program by name. Returns false if the
// requested name is not implemented by the desktop backend.
bool register_builtin_decl_program(DeclProgramRegistry &registry,
				   std::string_view name);

} // namespace declgl
