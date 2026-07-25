#include "elm_host/elm_adapter.h"

#include <cmath>
#include <string_view>
#include <unordered_set>

#include "transport_backend.pb.h"
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
			    JS_IsUndefined(child.get()))
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

} // namespace

bool command_from_js(JSContext *ctx, JSValueConst value,
		     const std::string &app_name,
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
		error = "execREGLCmd: custom shaders are not supported";
		return false;
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
		if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
			code = "Key" + key;
		else if (key == "Up")
			code = "ArrowUp";
		else if (key == "Down")
			code = "ArrowDown";
		else if (key == "Left")
			code = "ArrowLeft";
		else if (key == "Right")
			code = "ArrowRight";
		else if (key == "Return")
			code = "Enter";
		set(ctx, out, "key", JS_NewString(ctx, key.c_str()));
		set(ctx, out, "code", JS_NewString(ctx, code.c_str()));
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

} // namespace declgl::elm
