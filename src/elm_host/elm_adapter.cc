#include "elm_host/elm_adapter.h"

#include <cmath>
#include <string_view>
#include <unordered_set>

#include "transport_backend.pb.h"
#include "transport_audio.pb.h"
#include "transport_common.pb.h"
#include "transport_render.pb.h"

namespace declgl::elm
{
namespace
{

class Value {
    public:
	Value(JSContext *ctx, JSValue value)
		: ctx_(ctx)
		, value_(value)
	{
	}
	~Value()
	{
		JS_FreeValue(ctx_, value_);
	}
	Value(const Value &) = delete;
	Value &operator=(const Value &) = delete;
	Value(Value &&other) noexcept : ctx_(other.ctx_), value_(other.value_)
	{
		other.value_ = JS_UNDEFINED;
	}
	JSValueConst get() const
	{
		return value_;
	}
	JSValue release()
	{
		JSValue out = value_;
		value_ = JS_UNDEFINED;
		return out;
	}

    private:
	JSContext *ctx_;
	JSValue value_;
};

Value prop(JSContext *ctx, JSValueConst obj, const char *name)
{
	return Value(ctx, JS_GetPropertyStr(ctx, obj, name));
}

bool empty_object(JSContext *ctx, JSValueConst value)
{
	if (!JS_IsObject(value) || JS_IsArray(value))
		return false;
	JSPropertyEnum *properties = nullptr;
	uint32_t count = 0;
	if (JS_GetOwnPropertyNames(ctx, &properties, &count, value,
				   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
		return false;
	JS_FreePropertyEnum(ctx, properties, count);
	return count == 0;
}

int browser_key_code(std::string_view key)
{
	if (key.size() == 1) {
		const unsigned char c = static_cast<unsigned char>(key[0]);
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 'A';
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			return c;
		if (c == ' ')
			return 32;
	}
	if (key == "Backspace")
		return 8;
	if (key == "Tab")
		return 9;
	if (key == "Return" || key == "Enter")
		return 13;
	if (key == "Escape")
		return 27;
	if (key == "Space")
		return 32;
	if (key == "Left")
		return 37;
	if (key == "Up")
		return 38;
	if (key == "Right")
		return 39;
	if (key == "Down")
		return 40;
	if (key == "Delete")
		return 46;
	if (key.size() >= 2 && key[0] == 'F') {
		int number = 0;
		for (std::size_t i = 1; i < key.size(); ++i) {
			if (key[i] < '0' || key[i] > '9')
				return 0;
			number = number * 10 + key[i] - '0';
		}
		if (number >= 1 && number <= 24)
			return 111 + number;
	}
	return 0;
}

bool string_value(JSContext *ctx, JSValueConst value, std::string &out)
{
	if (!JS_IsString(value))
		return false;
	size_t len = 0;
	const char *s = JS_ToCStringLen(ctx, &len, value);
	if (!s)
		return false;
	out.assign(s, len);
	JS_FreeCString(ctx, s);
	return true;
}

bool number_value(JSContext *ctx, JSValueConst value, double &out)
{
	return JS_IsNumber(value) && JS_ToFloat64(ctx, &out, value) == 0 &&
	       std::isfinite(out);
}

bool required_string(JSContext *ctx, JSValueConst obj, const char *key,
		     const std::string &path, std::string &out,
		     std::string &error)
{
	Value v = prop(ctx, obj, key);
	if (string_value(ctx, v.get(), out))
		return true;
	error = path + "." + key + ": expected string";
	return false;
}

bool required_number(JSContext *ctx, JSValueConst obj, const char *key,
		     const std::string &path, double &out, std::string &error)
{
	Value v = prop(ctx, obj, key);
	if (number_value(ctx, v.get(), out))
		return true;
	error = path + "." + key + ": expected finite number";
	return false;
}

bool required_integer(JSContext *ctx, JSValueConst obj, const char *key,
		      const std::string &path, int64_t &out,
		      std::string &error)
{
	Value v = prop(ctx, obj, key);
	double n = 0;
	if (number_value(ctx, v.get(), n) && std::trunc(n) == n &&
	    n >= -9007199254740991.0 && n <= 9007199254740991.0) {
		out = static_cast<int64_t>(n);
		return true;
	}
	error = path + "." + key + ": expected integer";
	return false;
}

bool required_uint32(JSContext *ctx, JSValueConst obj, const char *key,
		     const std::string &path, uint32_t &out,
		     std::string &error)
{
	int64_t n = 0;
	if (!required_integer(ctx, obj, key, path, n, error))
		return false;
	if (n < 0 || n > UINT32_MAX) {
		error = path + "." + key + ": expected unsigned 32-bit integer";
		return false;
	}
	out = static_cast<uint32_t>(n);
	return true;
}

bool array_length(JSContext *ctx, JSValueConst value, int64_t &length)
{
	return JS_IsArray(value) && JS_GetLength(ctx, value, &length) == 0;
}

bool common_value(JSContext *ctx, JSValueConst value,
		  mlregl::transport::common::Value &out,
		  const std::string &path, std::string &error)
{
	if (JS_IsNumber(value)) {
		double n = 0;
		if (!number_value(ctx, value, n)) {
			error = path + ": expected finite number";
			return false;
		}
		out.set_number_value(n);
		return true;
	}
	if (JS_IsString(value)) {
		std::string s;
		if (!string_value(ctx, value, s))
			return false;
		out.set_string_value(std::move(s));
		return true;
	}
	if (JS_IsBool(value)) {
		out.set_bool_value(JS_ToBool(ctx, value) != 0);
		return true;
	}
	int64_t len = 0;
	if (!array_length(ctx, value, len)) {
		error = path +
			": expected number, string, boolean, or homogeneous array";
		return false;
	}
	if (len == 0) {
		out.mutable_number_array_value();
		return true;
	}
	Value first(ctx, JS_GetPropertyUint32(ctx, value, 0));
	if (JS_IsNumber(first.get())) {
		auto *array = out.mutable_number_array_value();
		for (int64_t i = 0; i < len; ++i) {
			Value item(ctx, JS_GetPropertyUint32(ctx, value, i));
			double n = 0;
			if (!number_value(ctx, item.get(), n)) {
				error = path + "[" + std::to_string(i) +
					"]: expected finite number";
				return false;
			}
			array->add_values(n);
		}
		return true;
	}
	if (JS_IsString(first.get())) {
		auto *array = out.mutable_string_array_value();
		for (int64_t i = 0; i < len; ++i) {
			Value item(ctx, JS_GetPropertyUint32(ctx, value, i));
			std::string s;
			if (!string_value(ctx, item.get(), s)) {
				error = path + "[" + std::to_string(i) +
					"]: expected string";
				return false;
			}
			array->add_values(std::move(s));
		}
		return true;
	}
	error = path + "[0]: arrays must contain only numbers or only strings";
	return false;
}

bool add_fields(JSContext *ctx, JSValueConst object,
		google::protobuf::RepeatedPtrField<
			mlregl::transport::render::ProgramCallField> *fields,
		const std::unordered_set<std::string> &skip,
		const std::string &path, std::string &error)
{
	JSPropertyEnum *properties = nullptr;
	uint32_t count = 0;
	if (JS_GetOwnPropertyNames(ctx, &properties, &count, object,
				   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
		error = path + ": cannot enumerate object";
		return false;
	}
	bool ok = true;
	for (uint32_t i = 0; i < count && ok; ++i) {
		const char *raw = JS_AtomToCString(ctx, properties[i].atom);
		if (!raw) {
			ok = false;
			break;
		}
		std::string key(raw);
		JS_FreeCString(ctx, raw);
		if (skip.count(key))
			continue;
		Value value(ctx,
			    JS_GetProperty(ctx, object, properties[i].atom));
		if (JS_IsNull(value.get()) || JS_IsUndefined(value.get()))
			continue;
		auto *field = fields->Add();
		field->set_key(key);
		ok = common_value(ctx, value.get(), *field->mutable_val(),
				  path + "." + key, error);
	}
	JS_FreePropertyEnum(ctx, properties, count);
	return ok;
}

enum class ProgramMappingKind { Static, Dynamic, DynamicTexture };

bool add_program_mappings(
	JSContext *ctx, JSValueConst object, ProgramMappingKind kind,
	google::protobuf::RepeatedPtrField<
		mlregl::transport::backend::ProgramValueMapping> *mappings,
	const std::string &path, std::string &error)
{
	if (JS_IsUndefined(object) || JS_IsNull(object))
		return true;
	if (!JS_IsObject(object) || JS_IsArray(object)) {
		error = path + ": expected object";
		return false;
	}
	JSPropertyEnum *properties = nullptr;
	uint32_t count = 0;
	if (JS_GetOwnPropertyNames(ctx, &properties, &count, object,
				   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
		error = path + ": cannot enumerate object";
		return false;
	}
	bool ok = true;
	for (uint32_t i = 0; i < count && ok; ++i) {
		const char *raw = JS_AtomToCString(ctx, properties[i].atom);
		if (!raw) {
			ok = false;
			break;
		}
		std::string key(raw);
		JS_FreeCString(ctx, raw);
		Value value(ctx, JS_GetProperty(ctx, object, properties[i].atom));
		auto *mapping = mappings->Add();
		mapping->set_key(key);
		if (kind == ProgramMappingKind::Static) {
			ok = common_value(ctx, value.get(),
					  *mapping->mutable_val()->mutable_static_val(),
					  path + "." + key, error);
		} else {
			std::string property;
			if (!string_value(ctx, value.get(), property)) {
				error = path + "." + key + ": expected string";
				ok = false;
			} else if (kind == ProgramMappingKind::Dynamic) {
				mapping->mutable_val()->set_dyn_val(std::move(property));
			} else {
				mapping->mutable_val()->set_dyn_textval(
					std::move(property));
			}
		}
	}
	JS_FreePropertyEnum(ctx, properties, count);
	return ok;
}

bool program_value(JSContext *ctx, JSValueConst program, const char *static_key,
		   const char *dynamic_key,
		   mlregl::transport::backend::ProgramValue *out,
		   std::string &error)
{
	Value dynamic = prop(ctx, program, dynamic_key);
	if (!JS_IsUndefined(dynamic.get()) && !JS_IsNull(dynamic.get())) {
		std::string property;
		if (!string_value(ctx, dynamic.get(), property)) {
			error = std::string("execREGLCmd.proto.") + dynamic_key +
				": expected string";
			return false;
		}
		out->set_dyn_val(std::move(property));
		return true;
	}
	Value value = prop(ctx, program, static_key);
	if (JS_IsUndefined(value.get()) || JS_IsNull(value.get()))
		return true;
	return common_value(ctx, value.get(), *out->mutable_static_val(),
			    std::string("execREGLCmd.proto.") + static_key,
			    error);
}

bool custom_program(JSContext *ctx, JSValueConst value,
		    mlregl::transport::backend::CreateProgram &out,
		    std::string &error)
{
	std::string name;
	if (!required_string(ctx, value, "_n", "execREGLCmd", name, error))
		return false;
	Value proto = prop(ctx, value, "proto");
	if (!JS_IsObject(proto.get()) || JS_IsArray(proto.get())) {
		error = "execREGLCmd.proto: expected object";
		return false;
	}
	std::string frag, vert;
	if (!required_string(ctx, proto.get(), "frag", "execREGLCmd.proto",
			     frag, error) ||
	    !required_string(ctx, proto.get(), "vert", "execREGLCmd.proto",
			     vert, error))
		return false;
	out.set_name(std::move(name));
	auto *program = out.mutable_program();
	program->set_frag(std::move(frag));
	program->set_vert(std::move(vert));
	program->set_shader_language(
		mlregl::transport::backend::SHADER_LANGUAGE_GLSL_ES_100);

	auto add = [&](const char *key, ProgramMappingKind kind,
		       auto *mappings) {
		Value object = prop(ctx, proto.get(), key);
		return add_program_mappings(ctx, object.get(), kind, mappings,
					    std::string("execREGLCmd.proto.") + key,
					    error);
	};
	if (!add("uniforms", ProgramMappingKind::Static,
		 program->mutable_uniforms()) ||
	    !add("uniformsDyn", ProgramMappingKind::Dynamic,
		 program->mutable_uniforms()) ||
	    !add("uniformsDynTexture", ProgramMappingKind::DynamicTexture,
		 program->mutable_uniforms()) ||
	    !add("attributes", ProgramMappingKind::Static,
		 program->mutable_attributes()) ||
	    !add("attributesDyn", ProgramMappingKind::Dynamic,
		 program->mutable_attributes()))
		return false;
	return program_value(ctx, proto.get(), "primitive", "primitiveDyn",
			     program->mutable_primitive(), error) &&
	       program_value(ctx, proto.get(), "elements", "elementsDyn",
			     program->mutable_elements(), error) &&
	       program_value(ctx, proto.get(), "count", "countDyn",
			     program->mutable_count(), error);
}

bool atomic(JSContext *ctx, JSValueConst value,
	    mlregl::transport::render::AtomicRenderable &out,
	    const std::string &path, bool clear, std::string &error)
{
	if (clear) {
		std::string name;
		if (!required_string(ctx, value, "_n", path, name, error))
			return false;
		if (name != "clear") {
			error = path + "._n: unsupported REGL command '" +
				name + "'";
			return false;
		}
		out.set_program("clear");
	} else {
		std::string program;
		if (!required_string(ctx, value, "_p", path, program, error))
			return false;
		out.set_program(std::move(program));
	}
	return add_fields(ctx, value, out.mutable_fields(),
			  { "_c", "_p", "_n" }, path, error);
}

bool renderable(JSContext *ctx, JSValueConst value,
		mlregl::transport::render::Renderable &out,
		const std::string &path, std::string &error)
{
	if (!JS_IsObject(value) || JS_IsArray(value)) {
		error = path + ": expected render object";
		return false;
	}
	Value kind_value = prop(ctx, value, "_c");
	double kind_number = 0;
	if (!number_value(ctx, kind_value.get(), kind_number)) {
		error = path + "._c: expected numeric render node kind";
		return false;
	}
	const int kind = static_cast<int>(kind_number);
	if (kind == 0 || kind == 1)
		return atomic(ctx, value, *out.mutable_atomic(), path,
			      kind == 1, error);
	if (kind == 4) {
		error = path + ": save-as-texture (_c = 4) is not supported";
		return false;
	}
	if (kind == 2) {
		auto *group = out.mutable_group();
		Value children = prop(ctx, value, "c");
		int64_t count = 0;
		if (!array_length(ctx, children.get(), count)) {
			error = path + ".c: expected array";
			return false;
		}
		for (int64_t i = 0; i < count; ++i) {
			Value child(ctx, JS_GetPropertyUint32(
						 ctx, children.get(), i));
			if (JS_IsNull(child.get()) ||
			    JS_IsUndefined(child.get()) ||
			    empty_object(ctx, child.get()))
				continue;
			if (!renderable(ctx, child.get(),
					*group->add_children(),
					path + ".c[" + std::to_string(i) + "]",
					error))
				return false;
		}
		Value effects = prop(ctx, value, "e");
		if (!array_length(ctx, effects.get(), count)) {
			error = path + ".e: expected array";
			return false;
		}
		for (int64_t i = 0; i < count; ++i) {
			Value effect(ctx, JS_GetPropertyUint32(
						  ctx, effects.get(), i));
			auto *native = group->add_effects();
			const std::string ep =
				path + ".e[" + std::to_string(i) + "]";
			std::string program;
			if (!required_string(ctx, effect.get(), "_p", ep,
					     program, error))
				return false;
			native->set_program(std::move(program));
			if (!add_fields(ctx, effect.get(),
					native->mutable_fields(), { "_p" }, ep,
					error))
				return false;
		}
		Value camera = prop(ctx, value, "_sc");
		if (!JS_IsUndefined(camera.get())) {
			if (!array_length(ctx, camera.get(), count) ||
			    count != 4) {
				error = path +
					"._sc: expected four-number camera array";
				return false;
			}
			double values[4];
			for (int i = 0; i < 4; ++i) {
				Value item(ctx, JS_GetPropertyUint32(
							ctx, camera.get(), i));
				if (!number_value(ctx, item.get(), values[i])) {
					error = path + "._sc[" +
						std::to_string(i) +
						"]: expected number";
					return false;
				}
			}
			auto *c = group->mutable_camera();
			c->set_x(values[0]);
			c->set_y(values[1]);
			c->set_zoom(values[2]);
			c->set_rotation(values[3]);
		}
		return true;
	}
	if (kind == 3) {
		auto *composite = out.mutable_composite();
		std::string program;
		if (!required_string(ctx, value, "_p", path, program, error))
			return false;
		composite->mutable_compositor()->set_program(
			std::move(program));
		if (!add_fields(
			    ctx, value,
			    composite->mutable_compositor()->mutable_fields(),
			    { "_c", "_p", "r1", "r2" }, path, error))
			return false;
		Value left = prop(ctx, value, "r1");
		Value right = prop(ctx, value, "r2");
		return renderable(ctx, left.get(), *composite->mutable_left(),
				  path + ".r1", error) &&
		       renderable(ctx, right.get(), *composite->mutable_right(),
				  path + ".r2", error);
	}
	error = path + "._c: unsupported render node kind " +
		std::to_string(kind);
	return false;
}

JSValue object(JSContext *ctx)
{
	return JS_NewObject(ctx);
}
void set(JSContext *ctx, JSValueConst obj, const char *key, JSValue value)
{
	JS_SetPropertyStr(ctx, obj, key, value);
}

bool volume_timelines(
	JSContext *ctx, JSValueConst value,
	google::protobuf::RepeatedPtrField<
		mlregl::transport::audio::VolumeTimeline> *out,
	const std::string &path, std::string &error)
{
	int64_t count = 0;
	if (!array_length(ctx, value, count)) {
		error = path + ": expected array";
		return false;
	}
	for (int64_t i = 0; i < count; ++i) {
		Value timeline(ctx, JS_GetPropertyUint32(ctx, value, i));
		int64_t points = 0;
		if (!array_length(ctx, timeline.get(), points)) {
			error = path + "[" + std::to_string(i) + "]: expected array";
			return false;
		}
		auto *native_timeline = out->Add();
		for (int64_t j = 0; j < points; ++j) {
			Value point(ctx, JS_GetPropertyUint32(ctx, timeline.get(), j));
			double time = 0, volume = 0;
			const std::string pp = path + "[" + std::to_string(i) +
					       "][" + std::to_string(j) + "]";
			if (!JS_IsObject(point.get()) ||
			    !required_number(ctx, point.get(), "time", pp, time,
					     error) ||
			    !required_number(ctx, point.get(), "volume", pp, volume,
					     error))
				return false;
			auto *p = native_timeline->add_points();
			p->set_time(time);
			p->set_volume(volume);
		}
	}
	return true;
}

bool loop_config(JSContext *ctx, JSValueConst value,
		 mlregl::transport::audio::LoopConfig *out,
		 const std::string &path, std::string &error)
{
	if (JS_IsNull(value))
		return true;
	if (!JS_IsObject(value)) {
		error = path + ": expected object or null";
		return false;
	}
	double start = 0, end = 0;
	if (!required_number(ctx, value, "loopStart", path, start, error) ||
	    !required_number(ctx, value, "loopEnd", path, end, error))
		return false;
	out->set_loop_start(start);
	out->set_loop_end(end);
	return true;
}

} // namespace

bool command_from_js(JSContext *ctx, JSValueConst value,
		     const std::string &app_name, bool fullscreen,
		     mlregl::transport::backend::BackendCommand &out,
		     std::string &error)
{
	if (!JS_IsObject(value) || JS_IsArray(value)) {
		error = "execREGLCmd: expected object";
		return false;
	}
	std::string kind;
	if (!required_string(ctx, value, "_c", "execREGLCmd", kind, error))
		return false;
	if (kind == "createGLProgram") {
		return custom_program(ctx, value, *out.mutable_create_program(),
				      error);
	}
	if (kind == "start") {
		double width, height, fbo;
		if (!required_number(ctx, value, "virtWidth", "execREGLCmd",
				     width, error) ||
		    !required_number(ctx, value, "virtHeight", "execREGLCmd",
				     height, error) ||
		    !required_number(ctx, value, "fboNum", "execREGLCmd", fbo,
				     error))
			return false;
		auto *start = out.mutable_start_regl();
		start->set_virt_width(width);
		start->set_virt_height(height);
		start->set_fbo_num(static_cast<uint32_t>(fbo));
		start->set_app_name(app_name);
		if (fullscreen)
			start->mutable_window()->set_fullscreen(true);
		Value programs = prop(ctx, value, "programs");
		if (!JS_IsUndefined(programs.get())) {
			int64_t count = 0;
			if (!array_length(ctx, programs.get(), count)) {
				error = "execREGLCmd.programs: expected array";
				return false;
			}
			for (int64_t i = 0; i < count; ++i) {
				Value item(ctx,
					   JS_GetPropertyUint32(
						   ctx, programs.get(), i));
				std::string name;
				if (!string_value(ctx, item.get(), name)) {
					error = "execREGLCmd.programs: expected strings";
					return false;
				}
				start->mutable_builtin_programs()->add_values(
					std::move(name));
			}
		}
		return true;
	}
	if (kind == "config") {
		Value config = prop(ctx, value, "config");
		double interval;
		if (!required_number(ctx, config.get(), "interval",
				     "execREGLCmd.config", interval, error))
			return false;
		out.mutable_config_regl()->set_interval_ms(interval);
		return true;
	}
	if (kind == "loadTexture") {
		std::string name;
		if (!required_string(ctx, value, "_n", "execREGLCmd", name,
				     error))
			return false;
		Value opts = prop(ctx, value, "opts");
		std::string path;
		if (!required_string(ctx, opts.get(), "data",
				     "execREGLCmd.opts", path, error))
			return false;
		auto *load = out.mutable_load_texture();
		load->set_name(name);
		load->set_url(path);
		// ml-regl-js sets flipY=true for every ordinary Image upload. Carry
		// that source-runtime convention explicitly instead of changing the
		// native desktop default for other protocol clients.
		load->mutable_options()->set_flip_y(true);
		std::string filter;
		Value mag = prop(ctx, opts.get(), "mag");
		if (string_value(ctx, mag.get(), filter) && filter == "nearest")
			load->mutable_options()->set_mag(
				mlregl::transport::backend::
					TEXTURE_MAG_OPTION_NEAREST);
		Value min = prop(ctx, opts.get(), "min");
		if (string_value(ctx, min.get(), filter)) {
			using namespace mlregl::transport::backend;
			if (filter == "nearest")
				load->mutable_options()->set_min(
					TEXTURE_MIN_OPTION_NEAREST);
			else if (filter == "nearest mipmap nearest")
				load->mutable_options()->set_min(
					TEXTURE_MIN_OPTION_NEAREST_MIPMAP_NEAREST);
			else if (filter == "linear mipmap nearest")
				load->mutable_options()->set_min(
					TEXTURE_MIN_OPTION_LINEAR_MIPMAP_NEAREST);
			else if (filter == "nearest mipmap linear")
				load->mutable_options()->set_min(
					TEXTURE_MIN_OPTION_NEAREST_MIPMAP_LINEAR);
			else if (filter == "linear mipmap linear")
				load->mutable_options()->set_min(
					TEXTURE_MIN_OPTION_LINEAR_MIPMAP_LINEAR);
		}
		Value crop = prop(ctx, opts.get(), "subimg");
		int64_t count = 0;
		if (!JS_IsNull(crop.get()) && !JS_IsUndefined(crop.get())) {
			if (!array_length(ctx, crop.get(), count) ||
			    count != 4) {
				error = "execREGLCmd.opts.subimg: expected four numbers";
				return false;
			}
			double n[4];
			for (int i = 0; i < 4; ++i) {
				Value item(ctx, JS_GetPropertyUint32(
							ctx, crop.get(), i));
				if (!number_value(ctx, item.get(), n[i])) {
					error = "execREGLCmd.opts.subimg[" +
						std::to_string(i) +
						"]: expected finite number";
					return false;
				}
			}
			auto *c = load->mutable_options()->mutable_crop();
			c->set_x(n[0]);
			c->set_y(n[1]);
			c->set_width(n[2]);
			c->set_height(n[3]);
		}
		return true;
	}
	if (kind == "loadFont") {
		std::string name, image, json;
		if (!required_string(ctx, value, "_n", "execREGLCmd", name,
				     error) ||
		    !required_string(ctx, value, "img", "execREGLCmd", image,
				     error) ||
		    !required_string(ctx, value, "json", "execREGLCmd", json,
				     error))
			return false;
		auto *load = out.mutable_load_font();
		load->set_name(name);
		load->set_image_url(image);
		load->set_json_url(json);
		return true;
	}
	error = "execREGLCmd._c: unsupported command '" + kind + "'";
	return false;
}

bool view_from_js(JSContext *ctx, JSValueConst value,
		  mlregl::transport::render::Renderable &out,
		  std::string &error)
{
	if (empty_object(ctx, value)) {
		out.Clear();
		return true;
	}
	return renderable(ctx, value, out, "setView", error);
}

JSValue
backend_event_to_js(JSContext *ctx,
		    const mlregl::transport::backend::BackendEvent &event,
		    std::string &error)
{
	JSValue root = object(ctx), response = object(ctx);
	using E = mlregl::transport::backend::BackendEvent;
	switch (event.kind_case()) {
	case E::kTextureLoaded:
		set(ctx, root, "_c", JS_NewString(ctx, "loadTexture"));
		set(ctx, response, "texture",
		    JS_NewString(ctx, event.texture_loaded().name().c_str()));
		set(ctx, response, "width",
		    JS_NewInt32(ctx, event.texture_loaded().width()));
		set(ctx, response, "height",
		    JS_NewInt32(ctx, event.texture_loaded().height()));
		break;
	case E::kFontLoaded:
		set(ctx, root, "_c", JS_NewString(ctx, "loadFont"));
		set(ctx, response, "font",
		    JS_NewString(ctx, event.font_loaded().name().c_str()));
		break;
	case E::kProgramCreated:
		set(ctx, root, "_c", JS_NewString(ctx, "createGLProgram"));
		set(ctx, response, "_n",
		    JS_NewString(ctx, event.program_created().name().c_str()));
		break;
	case E::kTextureLoadfail:
		error = "texture '" + event.texture_loadfail().name() +
			"' failed: " + event.texture_loadfail().reason();
		JS_FreeValue(ctx, root);
		JS_FreeValue(ctx, response);
		return JS_EXCEPTION;
	case E::kFontLoadfail:
		error = "font '" + event.font_loadfail().name() +
			"' failed: " + event.font_loadfail().reason();
		JS_FreeValue(ctx, root);
		JS_FreeValue(ctx, response);
		return JS_EXCEPTION;
	case E::kProgramCreatefail:
		error = "custom program '" + event.program_createfail().name() +
			"' failed to compile";
		JS_FreeValue(ctx, root);
		JS_FreeValue(ctx, response);
		return JS_EXCEPTION;
	default:
		error = "unsupported native backend event";
		JS_FreeValue(ctx, root);
		JS_FreeValue(ctx, response);
		return JS_EXCEPTION;
	}
	set(ctx, root, "response", response);
	return root;
}

JSValue input_event_to_js(JSContext *ctx,
			  const mlregl::transport::backend::Event &event,
			  std::string &type, std::string &error)
{
	JSValue out = object(ctx);
	auto set_modifiers = [&] {
		set(ctx, out, "ctrlKey", JS_NewBool(ctx, false));
		set(ctx, out, "shiftKey", JS_NewBool(ctx, false));
		set(ctx, out, "altKey", JS_NewBool(ctx, false));
		set(ctx, out, "metaKey", JS_NewBool(ctx, false));
		set(ctx, out, "timeStamp", JS_NewFloat64(ctx, 0));
	};
	using E = mlregl::transport::backend::Event;
	switch (event.kind_case()) {
	case E::kUpdateTick:
		type = "tick";
		set(ctx, out, "ts",
		    JS_NewFloat64(ctx, event.update_tick().ts()));
		break;
	case E::kMouseDown:
	case E::kMouseUp: {
		const auto &m = event.kind_case() == E::kMouseDown ?
					event.mouse_down() :
					event.mouse_up();
		type = event.kind_case() == E::kMouseDown ? "mousedown" :
							    "mouseup";
		set(ctx, out, "button",
		    JS_NewInt32(ctx, m.button() > 0 ? m.button() - 1 : 0));
		set(ctx, out, "clientX", JS_NewFloat64(ctx, m.x()));
		set(ctx, out, "clientY", JS_NewFloat64(ctx, m.y()));
		set(ctx, out, "offsetX", JS_NewFloat64(ctx, m.x()));
		set(ctx, out, "offsetY", JS_NewFloat64(ctx, m.y()));
		set(ctx, out, "pageX", JS_NewFloat64(ctx, m.x()));
		set(ctx, out, "pageY", JS_NewFloat64(ctx, m.y()));
		set(ctx, out, "buttons",
		    JS_NewInt32(ctx,
				event.kind_case() == E::kMouseDown ? 1 : 0));
		set(ctx, out, "detail", JS_NewInt32(ctx, 1));
		break;
	}
	case E::kMouseMove:
		type = "mousemove";
		set(ctx, out, "clientX",
		    JS_NewFloat64(ctx, event.mouse_move().x()));
		set(ctx, out, "clientY",
		    JS_NewFloat64(ctx, event.mouse_move().y()));
		set(ctx, out, "offsetX",
		    JS_NewFloat64(ctx, event.mouse_move().x()));
		set(ctx, out, "offsetY",
		    JS_NewFloat64(ctx, event.mouse_move().y()));
		set(ctx, out, "pageX",
		    JS_NewFloat64(ctx, event.mouse_move().x()));
		set(ctx, out, "pageY",
		    JS_NewFloat64(ctx, event.mouse_move().y()));
		set(ctx, out, "movementX", JS_NewInt32(ctx, 0));
		set(ctx, out, "movementY", JS_NewInt32(ctx, 0));
		break;
	case E::kKeyDown:
	case E::kKeyUp: {
		type = event.kind_case() == E::kKeyDown ? "keydown" : "keyup";
		const std::string &key = event.kind_case() == E::kKeyDown ?
						 event.key_down().code() :
						 event.key_up().code();
		std::string code = key;
		std::string browser_key = key;
		if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
			code = "Key" + key;
		else if (key == "Up") {
			code = "ArrowUp";
			browser_key = code;
		} else if (key == "Down") {
			code = "ArrowDown";
			browser_key = code;
		} else if (key == "Left") {
			code = "ArrowLeft";
			browser_key = code;
		} else if (key == "Right") {
			code = "ArrowRight";
			browser_key = code;
		} else if (key == "Return") {
			code = "Enter";
			browser_key = "Enter";
		} else if (key == "Space") {
			code = "Space";
			browser_key = " ";
		}
		const int key_code = browser_key_code(key);
		set(ctx, out, "key", JS_NewString(ctx, browser_key.c_str()));
		set(ctx, out, "code", JS_NewString(ctx, code.c_str()));
		set(ctx, out, "keyCode", JS_NewInt32(ctx, key_code));
		set(ctx, out, "which", JS_NewInt32(ctx, key_code));
		set(ctx, out, "repeat", JS_NewBool(ctx, false));
		set(ctx, out, "location", JS_NewInt32(ctx, 0));
		break;
	}
	default:
		error = "unknown input event";
		JS_FreeValue(ctx, out);
		return JS_EXCEPTION;
	}
	set(ctx, out, "bubbles", JS_NewBool(ctx, true));
	set(ctx, out, "cancelable", JS_NewBool(ctx, true));
	set_modifiers();
	return out;
}

bool audio_batch_from_js(
	JSContext *ctx, JSValueConst value,
	mlregl::transport::audio::AudioCommandBatch &audio,
	mlregl::transport::backend::BackendCommandBatch &loads,
	std::vector<AudioLoadRequest> &requests, std::string &error)
{
	if (!JS_IsObject(value) || JS_IsArray(value)) {
		error = "audioPortToJS: expected object";
		return false;
	}
	Value actions_value = prop(ctx, value, "audio");
	int64_t action_count = 0;
	if (!array_length(ctx, actions_value.get(), action_count)) {
		error = "audioPortToJS.audio: expected array";
		return false;
	}
	for (int64_t i = 0; i < action_count; ++i) {
		Value value_action(
			ctx, JS_GetPropertyUint32(ctx, actions_value.get(), i));
		const std::string path =
			"audioPortToJS.audio[" + std::to_string(i) + "]";
		if (!JS_IsObject(value_action.get())) {
			error = path + ": expected object";
			return false;
		}
		std::string kind;
		uint32_t group = 0;
		if (!required_string(ctx, value_action.get(), "action", path,
				     kind, error) ||
		    !required_uint32(ctx, value_action.get(), "nodeGroupId", path,
				     group, error))
			return false;
		auto *action = audio.add_actions();
		if (kind == "startSound") {
			uint32_t buffer = 0;
			double start_time = 0, start_at = 0, volume = 0,
			       playback_rate = 0;
			if (!required_uint32(ctx, value_action.get(), "bufferId",
					     path, buffer, error) ||
			    !required_number(ctx, value_action.get(), "startTime",
					     path, start_time, error) ||
			    !required_number(ctx, value_action.get(), "startAt", path,
					     start_at, error) ||
			    !required_number(ctx, value_action.get(), "volume", path,
					     volume, error) ||
			    !required_number(ctx, value_action.get(), "playbackRate",
					     path, playback_rate, error))
				return false;
			auto *start = action->mutable_start_sound();
			start->set_node_group_id(group);
			start->set_buffer_id(buffer);
			start->set_start_time(start_time);
			start->set_start_at(start_at);
			start->set_volume(volume);
			start->set_playback_rate(playback_rate);
			Value timelines = prop(ctx, value_action.get(),
					       "volumeTimelines");
			if (!volume_timelines(ctx, timelines.get(),
					      start->mutable_volume_timelines(),
					      path + ".volumeTimelines", error))
				return false;
			Value loop = prop(ctx, value_action.get(), "loop");
			if (!JS_IsNull(loop.get()) &&
			    !loop_config(ctx, loop.get(), start->mutable_loop(),
					 path + ".loop", error))
				return false;
		} else if (kind == "stopSound") {
			action->mutable_stop_sound()->set_node_group_id(group);
		} else if (kind == "setVolume") {
			double volume = 0;
			if (!required_number(ctx, value_action.get(), "volume", path,
					     volume, error))
				return false;
			auto *set_volume = action->mutable_set_volume();
			set_volume->set_node_group_id(group);
			set_volume->set_volume(volume);
		} else if (kind == "setVolumeAt") {
			auto *set_at = action->mutable_set_volume_at();
			set_at->set_node_group_id(group);
			Value timelines = prop(ctx, value_action.get(), "volumeAt");
			if (!volume_timelines(ctx, timelines.get(),
					      set_at->mutable_volume_at(),
					      path + ".volumeAt", error))
				return false;
		} else if (kind == "setLoopConfig") {
			auto *set_loop = action->mutable_set_loop_config();
			set_loop->set_node_group_id(group);
			Value loop = prop(ctx, value_action.get(), "loop");
			if (!JS_IsNull(loop.get()) &&
			    !loop_config(ctx, loop.get(), set_loop->mutable_loop(),
					 path + ".loop", error))
				return false;
		} else if (kind == "setPlaybackRate") {
			double rate = 0;
			if (!required_number(ctx, value_action.get(), "playbackRate",
					     path, rate, error))
				return false;
			auto *set_rate = action->mutable_set_playback_rate();
			set_rate->set_node_group_id(group);
			set_rate->set_playback_rate(rate);
		} else {
			error = path + ".action: unsupported action '" + kind + "'";
			return false;
		}
	}

	Value commands = prop(ctx, value, "audioCmds");
	int64_t command_count = 0;
	if (!array_length(ctx, commands.get(), command_count)) {
		error = "audioPortToJS.audioCmds: expected array";
		return false;
	}
	for (int64_t i = 0; i < command_count; ++i) {
		Value request(ctx, JS_GetPropertyUint32(ctx, commands.get(), i));
		const std::string path =
			"audioPortToJS.audioCmds[" + std::to_string(i) + "]";
		std::string url;
		int64_t request_id = 0;
		if (!JS_IsObject(request.get()) ||
		    !required_string(ctx, request.get(), "audioUrl", path, url,
				     error) ||
		    !required_integer(ctx, request.get(), "requestId", path,
				      request_id, error))
			return false;
		loads.add_commands()->mutable_load_audio()->set_audio_url(url);
		requests.push_back({ std::move(url), request_id });
	}
	return true;
}

JSValue audio_event_to_js(
	JSContext *ctx,
	const mlregl::transport::audio::AudioBackendEvent &event,
	std::optional<int64_t> request_id, std::string &error)
{
	JSValue out = object(ctx);
	using E = mlregl::transport::audio::AudioBackendEvent;
	switch (event.kind_case()) {
	case E::kAudioContextReady:
		set(ctx, out, "type", JS_NewInt32(ctx, 2));
		set(ctx, out, "samplesPerSecond",
		    JS_NewInt32(ctx, event.audio_context_ready().sample_rate()));
		break;
	case E::kAudioLoadSuccess:
		if (!request_id) {
			error = "audio load success has no pending Elm request";
			JS_FreeValue(ctx, out);
			return JS_EXCEPTION;
		}
		set(ctx, out, "type", JS_NewInt32(ctx, 1));
		set(ctx, out, "requestId", JS_NewInt64(ctx, *request_id));
		set(ctx, out, "bufferId",
		    JS_NewInt32(ctx, event.audio_load_success().buffer_id()));
		set(ctx, out, "durationInSeconds",
		    JS_NewFloat64(ctx, event.audio_load_success().duration()));
		break;
	case E::kAudioLoadFailed: {
		if (!request_id) {
			error = "audio load failure has no pending Elm request";
			JS_FreeValue(ctx, out);
			return JS_EXCEPTION;
		}
		set(ctx, out, "type", JS_NewInt32(ctx, 0));
		set(ctx, out, "requestId", JS_NewInt64(ctx, *request_id));
		const char *message = "UnknownError";
		if (event.audio_load_failed().error() ==
		    mlregl::transport::audio::AUDIO_LOAD_ERROR_NETWORK)
			message = "NetworkError";
		else if (event.audio_load_failed().error() ==
			 mlregl::transport::audio::AUDIO_LOAD_ERROR_FAILED_TO_DECODE)
			message = "MediaDecodeAudioDataUnknownContentType";
		set(ctx, out, "error", JS_NewString(ctx, message));
		break;
	}
	default:
		error = "unsupported native audio event";
		JS_FreeValue(ctx, out);
		return JS_EXCEPTION;
	}
	return out;
}

} // namespace declgl::elm
