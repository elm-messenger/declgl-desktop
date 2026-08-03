// renderer/programs/dynamic_program.cc

#include "renderer/programs/dynamic_program.h"

#include <algorithm>

#include "log/log.h"
#include "renderer/programs/glsl_es_translator.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl
{
namespace programs
{

namespace
{

using mlregl::transport::common::Value;

void set_numeric_uniform(DrawState &state, GLint loc,
			 const std::vector<float> &values)
{
	switch (values.size()) {
	case 1:
		state.set_uniform_f1(loc, values[0]);
		break;
	case 2:
		state.set_uniform_f2(loc, values[0], values[1]);
		break;
	case 3:
		state.set_uniform_f3(loc, values[0], values[1], values[2]);
		break;
	case 4:
		state.set_uniform_f4(loc, values[0], values[1], values[2],
				     values[3]);
		break;
	default:
		break;
	}
}

const ProgramCallField *find_dyn_field(const ProgramCallFields &fields,
				       std::string_view key)
{
	return find_field(fields, key);
}

} // namespace

DynamicProgram::DynamicProgram(std::string name, const BackendProgram &program)
	: name_(std::move(name))
{
	using Language = mlregl::transport::backend::ShaderLanguage;
	std::optional<GlslEs100Identifiers> es_identifiers;
	if (program.shader_language() ==
	    Language::SHADER_LANGUAGE_GLSL_ES_100) {
		es_identifiers = choose_glsl_es_100_identifiers(program.vert(),
								program.frag());
		if (!translate_glsl_es_100(program.vert(), ShaderStage::Vertex,
					   *es_identifiers, vert_src_,
					   translation_error_)) {
			translation_error_ =
				"vertex shader: " + translation_error_;
		} else if (!translate_glsl_es_100(program.frag(),
						  ShaderStage::Fragment,
						  *es_identifiers, frag_src_,
						  translation_error_)) {
			translation_error_ =
				"fragment shader: " + translation_error_;
		}
	} else if (program.shader_language() ==
		   Language::SHADER_LANGUAGE_GLSL) {
		vert_src_ = program.vert();
		frag_src_ = program.frag();
	} else {
		translation_error_ = "unsupported shader language " +
				     std::to_string(static_cast<int>(
					     program.shader_language()));
	}
	if (!translation_error_.empty())
		DECLGL_LOG_ERROR("dynamic program '{}': {}", name_,
				 translation_error_);

	uniforms_.reserve(program.uniforms_size());
	for (const auto &mapping : program.uniforms()) {
		Mapping value = make_mapping(mapping);
		if (es_identifiers) {
			value.key = translate_glsl_es_100_identifier(
				value.key, *es_identifiers);
		}
		uniforms_.push_back(std::move(value));
	}

	attributes_.reserve(program.attributes_size());
	for (const auto &mapping : program.attributes()) {
		Mapping value = make_mapping(mapping);
		if (es_identifiers) {
			value.key = translate_glsl_es_100_identifier(
				value.key, *es_identifiers);
		}
		attributes_.push_back(std::move(value));
	}

	if (program.has_primitive()) {
		primitive_ = make_pseudo_mapping(program.primitive());
	}
	if (program.has_elements()) {
		elements_ = make_pseudo_mapping(program.elements());
	}
	if (program.has_count()) {
		count_ = make_pseudo_mapping(program.count());
	}
}

bool DynamicProgram::after_compile()
{
	for (auto &uniform : uniforms_) {
		uniform.loc = uniform_location(uniform.key);
	}
	for (auto &attribute : attributes_) {
		attribute.loc = attribute_location(attribute.key);
	}
	return true;
}

DynamicProgram::Mapping DynamicProgram::make_mapping(
	const mlregl::transport::backend::ProgramValueMapping &mapping)
{
	Mapping out;
	out.key = mapping.key();
	if (mapping.has_val()) {
		Mapping value = make_pseudo_mapping(mapping.val());
		value.key = out.key;
		return value;
	}
	return out;
}

DynamicProgram::Mapping
DynamicProgram::make_pseudo_mapping(const ProgramValue &value)
{
	Mapping out;
	switch (value.val_case()) {
	case ProgramValue::kDynVal:
		out.kind = MappingKind::DynValue;
		out.prop = value.dyn_val();
		break;
	case ProgramValue::kDynTextval:
		out.kind = MappingKind::DynTexture;
		out.prop = value.dyn_textval();
		break;
	case ProgramValue::kStaticVal:
		out.kind = MappingKind::StaticValue;
		out.static_value = value.static_val();
		out.static_floats = value_as_floats(out.static_value);
		break;
	case ProgramValue::VAL_NOT_SET:
		break;
	}
	return out;
}

std::vector<float> DynamicProgram::value_as_floats(const Value &value)
{
	std::vector<float> out;
	switch (value.kind_case()) {
	case Value::kNumberValue:
		out.push_back(static_cast<float>(value.number_value()));
		break;
	case Value::kNumberArrayValue:
		out.reserve(value.number_array_value().values_size());
		for (double d : value.number_array_value().values()) {
			out.push_back(static_cast<float>(d));
		}
		break;
	case Value::kStringArrayValue:
		// Numeric uniforms / attributes can't accept a list of
		// strings. The textbox program reads StringArray fields
		// directly through its own decoder; for any other shader
		// this is a programmer error in the OCaml-side Program
		// definition. Surface it loudly instead of silently
		// rendering with an empty value vector.
		DECLGL_LOG_ERROR("DynamicProgram: numeric value expected, got "
				 "string_array_value with {} entries; "
				 "value will be dropped",
				 value.string_array_value().values_size());
		break;
	case Value::kStringValue:
	case Value::kBoolValue:
		// String/bool aren't numeric; callers handle these
		// upstream (string for textures/primitives, bool for
		// glUniform1i). Reaching this branch means a caller
		// asked for a numeric float vector from a non-numeric
		// value — a programmer error.
		DECLGL_LOG_ERROR("DynamicProgram: numeric value expected, got "
				 "{} value; value will be dropped",
				 value.kind_case() == Value::kStringValue ?
					 "string" :
					 "bool");
		break;
	case Value::KIND_NOT_SET:
		break;
	}
	return out;
}

GLenum DynamicProgram::primitive_from_string(const std::string &s)
{
	if (s == "points")
		return GL_POINTS;
	if (s == "lines")
		return GL_LINES;
	if (s == "line strip")
		return GL_LINE_STRIP;
	if (s == "line loop")
		return GL_LINE_LOOP;
	if (s == "triangles")
		return GL_TRIANGLES;
	if (s == "triangle strip")
		return GL_TRIANGLE_STRIP;
	if (s == "triangle fan")
		return GL_TRIANGLE_FAN;
	return GL_TRIANGLES;
}

int DynamicProgram::infer_components(std::string_view key, size_t value_count,
				     GLsizei explicit_count)
{
	if (explicit_count > 0 &&
	    value_count % static_cast<size_t>(explicit_count) == 0) {
		const size_t c =
			value_count / static_cast<size_t>(explicit_count);
		if (c >= 1 && c <= 4)
			return static_cast<int>(c);
	}

	if (key == "color" || key == "colour")
		return 4;
	if (key == "normal" && value_count % 3 == 0)
		return 3;
	return 2;
}

DynamicProgram::ResolvedValue
DynamicProgram::resolve_value(const Mapping &mapping,
			      const ProgramCallFields &fields) const
{
	ResolvedValue out;
	if (mapping.kind == MappingKind::StaticValue) {
		out.value = &mapping.static_value;
		out.static_storage = mapping.static_floats;
		return out;
	}

	const ProgramCallField *field = find_dyn_field(fields, mapping.prop);
	out.value = field && field->has_val() ? &field->val() : nullptr;
	return out;
}

bool DynamicProgram::apply_uniform(const Mapping &mapping,
				   const ProgramCallFields &fields,
				   const RenderContext &ctx,
				   const BuiltinTextures &builtin_textures,
				   DrawState &out_state) const
{
	if (mapping.kind == MappingKind::DynTexture) {
		const ProgramCallField *field =
			find_dyn_field(fields, mapping.prop);
		if (!field || !field->has_val() ||
		    field->val().kind_case() != Value::kStringValue) {
			return true;
		}
		if (!ctx.textures)
			return false;
		const Texture *tex =
			ctx.textures->get(field->val().string_value());
		if (!tex)
			return false;
		out_state.set_uniform_tex(mapping.loc, tex->id());
		return true;
	}

	ResolvedValue resolved = resolve_value(mapping, fields);
	if (!resolved.value) {
		// uniformsDyn maps an arbitrary GLSL uniform name (mapping.key/loc)
		// to a runtime property (mapping.prop). The render graph injects its
		// FBOs as the same texture/t1/t2 properties used by ml-regl-js.
		GLuint builtin_texture = 0;
		if (mapping.kind == MappingKind::DynValue) {
			if (mapping.prop == "texture")
				builtin_texture = builtin_textures.texture;
			else if (mapping.prop == "t1")
				builtin_texture = builtin_textures.t1;
			else if (mapping.prop == "t2")
				builtin_texture = builtin_textures.t2;
		}
		if (builtin_texture != 0)
			out_state.set_uniform_tex(mapping.loc, builtin_texture);
		return true;
	}

	if (resolved.value->kind_case() == Value::kBoolValue) {
		out_state.set_uniform_i1(mapping.loc,
					 resolved.value->bool_value() ? 1 : 0);
		return true;
	}

	const std::vector<float> values =
		mapping.kind == MappingKind::StaticValue ?
			mapping.static_floats :
			value_as_floats(*resolved.value);
	set_numeric_uniform(out_state, mapping.loc, values);
	return true;
}

bool DynamicProgram::resolve_primitive(const ProgramCallFields &fields,
				       GLenum &out_primitive) const
{
	if (!primitive_)
		return true;
	ResolvedValue resolved = resolve_value(*primitive_, fields);
	if (!resolved.value)
		return true;
	if (resolved.value->kind_case() == Value::kStringValue) {
		out_primitive =
			primitive_from_string(resolved.value->string_value());
	}
	return true;
}

bool DynamicProgram::resolve_count(const ProgramCallFields &fields,
				   GLsizei &out_count) const
{
	if (!count_)
		return true;
	ResolvedValue resolved = resolve_value(*count_, fields);
	if (!resolved.value)
		return true;
	if (resolved.value->kind_case() == Value::kNumberValue) {
		out_count =
			static_cast<GLsizei>(resolved.value->number_value());
	}
	return true;
}

bool DynamicProgram::resolve_indices(const ProgramCallFields &fields,
				     std::vector<uint32_t> &out_indices) const
{
	if (!elements_)
		return true;
	ResolvedValue resolved = resolve_value(*elements_, fields);
	if (!resolved.value)
		return true;
	const std::vector<float> values =
		elements_->kind == MappingKind::StaticValue ?
			elements_->static_floats :
			value_as_floats(*resolved.value);
	out_indices.reserve(values.size());
	for (float v : values) {
		out_indices.push_back(static_cast<uint32_t>(v));
	}
	return true;
}

bool DynamicProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures &builtin_textures,
			     DrawState &out_state)
{
	out_state.primitive = GL_TRIANGLES;
	resolve_primitive(fields, out_state.primitive);

	GLsizei explicit_count = 0;
	resolve_count(fields, explicit_count);

	set_builtin_uniforms(ctx, out_state);

	for (const auto &uniform : uniforms_) {
		if (!apply_uniform(uniform, fields, ctx, builtin_textures,
				   out_state)) {
			return false;
		}
	}

	for (const auto &attribute : attributes_) {
		ResolvedValue resolved = resolve_value(attribute, fields);
		if (!resolved.value &&
		    attribute.kind != MappingKind::StaticValue)
			continue;

		const std::vector<float> values =
			attribute.kind == MappingKind::StaticValue ?
				attribute.static_floats :
				value_as_floats(*resolved.value);
		if (values.empty())
			continue;

		const int components = infer_components(
			attribute.key, values.size(), explicit_count);
		const size_t vertex_count =
			values.size() / static_cast<size_t>(components);
		if (vertex_count == 0)
			continue;

		if (attribute.kind == MappingKind::StaticValue) {
			out_state.add_static_attrib(
				attribute.loc, components,
				attribute.static_floats.data(),
				static_cast<GLsizei>(vertex_count));
		} else {
			out_state.add_dyn_attrib(attribute.loc, components,
						 values.data(), vertex_count);
		}
	}

	std::vector<uint32_t> indices;
	resolve_indices(fields, indices);
	if (!indices.empty()) {
		out_state.indexed = true;
		out_state.indices = std::move(indices);
		out_state.count =
			explicit_count > 0 ?
				std::min(explicit_count,
					 static_cast<GLsizei>(
						 out_state.indices.size())) :
				static_cast<GLsizei>(out_state.indices.size());
	} else {
		out_state.indexed = false;
		out_state.count = explicit_count;
	}

	return !out_state.static_attribs.empty() ||
	       !out_state.dyn_attribs.empty() || out_state.count > 0;
}

} // namespace programs
} // namespace declgl
