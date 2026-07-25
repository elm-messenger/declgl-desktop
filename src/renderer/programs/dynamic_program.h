// renderer/programs/dynamic_program.h
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "renderer/program_base.h"
#include "transport_backend.pb.h"

namespace declgl
{
namespace programs
{

// Runtime-created program, mirroring ml-regl-js createGLProgram().
// The GLSL sources and mapping metadata come from CreateProgram.
class DynamicProgram : public ProgramBase {
    public:
	using BackendProgram = mlregl::transport::backend::Program;

	DynamicProgram(std::string name, const BackendProgram &program);
	bool valid() const
	{
		return translation_error_.empty();
	}
	const std::string &translation_error() const
	{
		return translation_error_;
	}

	std::string_view name() const override
	{
		return name_;
	}
	std::string_view vert_source() const override
	{
		return vert_src_;
	}
	std::string_view frag_source() const override
	{
		return frag_src_;
	}

	bool prepare(const ProgramCallFields &fields, const RenderContext &ctx,
		     const BuiltinTextures &builtin_textures,
		     DrawState &out_state) override;

    protected:
	bool after_compile() override;

    private:
	using ProgramValue = mlregl::transport::backend::ProgramValue;
	using Value = mlregl::transport::common::Value;

	enum class MappingKind { DynValue, DynTexture, StaticValue };

	struct Mapping {
		std::string key;
		GLint loc = -1;
		MappingKind kind = MappingKind::DynValue;
		std::string prop;
		Value static_value;
		std::vector<float> static_floats;
	};

	struct ResolvedValue {
		const Value *value = nullptr;
		std::vector<float> static_storage;
	};

	static Mapping make_mapping(
		const mlregl::transport::backend::ProgramValueMapping &mapping);
	static Mapping make_pseudo_mapping(const ProgramValue &value);
	static std::vector<float> value_as_floats(const Value &value);
	static GLenum primitive_from_string(const std::string &s);
	static int infer_components(std::string_view key, size_t value_count,
				    GLsizei explicit_count);

	ResolvedValue resolve_value(const Mapping &mapping,
				    const ProgramCallFields &fields) const;
	bool apply_uniform(const Mapping &mapping,
			   const ProgramCallFields &fields,
			   const RenderContext &ctx,
			   DrawState &out_state) const;
	bool resolve_primitive(const ProgramCallFields &fields,
			       GLenum &out_primitive) const;
	bool resolve_count(const ProgramCallFields &fields,
			   GLsizei &out_count) const;
	bool resolve_indices(const ProgramCallFields &fields,
			     std::vector<uint32_t> &out_indices) const;

	std::string name_;
	std::string vert_src_;
	std::string frag_src_;
	std::string translation_error_;
	std::vector<Mapping> uniforms_;
	std::vector<Mapping> attributes_;
	std::optional<Mapping> primitive_;
	std::optional<Mapping> elements_;
	std::optional<Mapping> count_;
};

} // namespace programs
} // namespace declgl
