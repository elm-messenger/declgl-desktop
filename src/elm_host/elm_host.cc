#include "elm_host/elm_host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "elm_host/elm_adapter.h"
#include "headless_dom.inc"
#include "log/log.h"
#include "transport_audio.pb.h"
#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace declgl
{
namespace
{

std::optional<std::string> read_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	std::ostringstream contents;
	contents << stream.rdbuf();
	return contents.str();
}

std::string js_string(JSContext *ctx, JSValueConst value)
{
	size_t len = 0;
	const char *raw = JS_ToCStringLen(ctx, &len, value);
	if (!raw)
		return {};
	std::string out(raw, len);
	JS_FreeCString(ctx, raw);
	return out;
}

void set_property(JSContext *ctx, JSValueConst object, const char *name,
		  JSValue value)
{
	JS_SetPropertyStr(ctx, object, name, value);
}

} // namespace

ElmHost::ElmHost(ElmHostConfig config)
	: config_(std::move(config))
	, started_(std::chrono::steady_clock::now())
{
	const auto wall = std::chrono::system_clock::now().time_since_epoch();
	time_origin_ms_ =
		std::chrono::duration<double, std::milli>(wall).count();
}

ElmHost::~ElmHost()
{
	if (!ctx_)
		return;
	for (auto &timer : timers_)
		JS_FreeValue(ctx_, timer.callback);
	timers_.clear();
	JS_FreeValue(ctx_, dispatch_fn_);
	JS_FreeValue(ctx_, data_file_port_);
	JS_FreeValue(ctx_, audio_from_port_);
	JS_FreeValue(ctx_, recv_port_);
	JS_FreeValue(ctx_, update_port_);
	JS_FreeValue(ctx_, app_);
	JS_FreeContext(ctx_);
	ctx_ = nullptr;
	JS_RunGC(rt_);
	JS_FreeRuntime(rt_);
	rt_ = nullptr;
}

ElmHost *ElmHost::self(JSContext *ctx)
{
	return static_cast<ElmHost *>(JS_GetContextOpaque(ctx));
}

double ElmHost::now_ms() const
{
	return std::chrono::duration<double, std::milli>(
		       std::chrono::steady_clock::now() - started_)
		.count();
}

int ElmHost::interrupt_handler(JSRuntime *, void *opaque)
{
	auto *host = static_cast<ElmHost *>(opaque);
	return std::chrono::steady_clock::now() > host->interrupt_deadline_;
}

void ElmHost::fail(std::string message)
{
	if (failed_)
		return;
	failed_ = true;
	error_ = std::move(message);
	DECLGL_LOG_ERROR("Elm runtime: {}", error_);
}

void ElmHost::capture_exception(const char *operation)
{
	JSValue exception = JS_GetException(ctx_);
	std::string message = js_string(ctx_, exception);
	JSValue stack = JS_GetPropertyStr(ctx_, exception, "stack");
	std::string stack_text = js_string(ctx_, stack);
	JS_FreeValue(ctx_, stack);
	JS_FreeValue(ctx_, exception);
	fail(std::string(operation) + ": " + message +
	     (stack_text.empty() ? "" : "\n" + stack_text));
}

bool ElmHost::eval_source(const char *source, std::size_t len, const char *name)
{
	interrupt_deadline_ =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	JSValue result = JS_Eval(ctx_, source, len, name, JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		capture_exception(name);
		return false;
	}
	JS_FreeValue(ctx_, result);
	return true;
}

bool ElmHost::eval_file(const std::filesystem::path &path)
{
	auto source = read_file(path);
	if (!source) {
		fail("cannot read JavaScript file '" + path.string() + "'");
		return false;
	}
	return eval_source(source->data(), source->size(),
			   path.string().c_str());
}

JSValue ElmHost::js_now(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
	return JS_NewFloat64(ctx, self(ctx)->now_ms());
}

JSValue ElmHost::js_time_origin(JSContext *ctx, JSValueConst, int,
				JSValueConst *)
{
	return JS_NewFloat64(ctx, self(ctx)->time_origin_ms_);
}

JSValue ElmHost::js_set_timer(JSContext *ctx, JSValueConst, int argc,
			      JSValueConst *argv)
{
	auto *host = self(ctx);
	if (argc < 3 || !JS_IsFunction(ctx, argv[0]))
		return JS_ThrowTypeError(
			ctx,
			"setTimer expects callback, delay, animationFrame");
	double delay = 0;
	JS_ToFloat64(ctx, &delay, argv[1]);
	Timer timer{ host->next_timer_id_++,
		     host->now_ms() + std::max(0.0, delay),
		     JS_ToBool(ctx, argv[2]) != 0, JS_DupValue(ctx, argv[0]) };
	host->timers_.push_back(timer);
	return JS_NewInt64(ctx, static_cast<int64_t>(timer.id));
}

JSValue ElmHost::js_clear_timer(JSContext *ctx, JSValueConst, int argc,
				JSValueConst *argv)
{
	auto *host = self(ctx);
	int64_t id = 0;
	if (argc > 0)
		JS_ToInt64(ctx, &id, argv[0]);
	for (auto it = host->timers_.begin(); it != host->timers_.end();) {
		if (it->id == static_cast<uint64_t>(id)) {
			JS_FreeValue(ctx, it->callback);
			it = host->timers_.erase(it);
		} else
			++it;
	}
	return JS_UNDEFINED;
}

JSValue ElmHost::js_log(JSContext *ctx, JSValueConst, int argc,
			JSValueConst *argv)
{
	const std::string level = argc > 0 ? js_string(ctx, argv[0]) : "info";
	const std::string message = argc > 1 ? js_string(ctx, argv[1]) : "";
	if (level == "error")
		DECLGL_LOG_ERROR("JS: {}", message);
	else if (level == "warn")
		DECLGL_LOG_WARN("JS: {}", message);
	else
		DECLGL_LOG_INFO("JS: {}", message);
	return JS_UNDEFINED;
}

JSValue ElmHost::js_command(JSContext *ctx, JSValueConst, int argc,
			    JSValueConst *argv)
{
	auto *host = self(ctx);
	if (argc < 1)
		return JS_ThrowTypeError(
			ctx, "execREGLCmd callback expects a value");
	mlregl::transport::backend::BackendCommand command;
	std::string error;
	if (!elm::command_from_js(ctx, argv[0], host->config_.app_name,
				  host->config_.fullscreen, command, error)) {
		host->fail(std::move(error));
		return JS_ThrowTypeError(ctx, "%s", host->error_.c_str());
	}
	const bool start =
		command.kind_case() ==
		mlregl::transport::backend::BackendCommand::kStartRegl;
	mlregl::transport::backend::BackendCommandBatch batch;
	*batch.add_commands() = std::move(command);
	std::string bytes;
	batch.SerializeToString(&bytes);
	host->enqueue_command(std::move(bytes), start);
	return JS_UNDEFINED;
}

JSValue ElmHost::js_view(JSContext *ctx, JSValueConst, int argc,
			 JSValueConst *argv)
{
	auto *host = self(ctx);
	if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
		host->latest_view_.reset();
		return JS_UNDEFINED;
	}
	mlregl::transport::render::Renderable renderable;
	std::string error;
	if (!elm::view_from_js(ctx, argv[0], renderable, error)) {
		host->fail(std::move(error));
		return JS_ThrowTypeError(ctx, "%s", host->error_.c_str());
	}
	std::string bytes;
	renderable.SerializeToString(&bytes);
	host->latest_view_ = std::vector<uint8_t>(bytes.begin(), bytes.end());
	return JS_UNDEFINED;
}

JSValue ElmHost::js_audio(JSContext *ctx, JSValueConst, int argc,
			  JSValueConst *argv)
{
	auto *host = self(ctx);
	if (argc < 1)
		return JS_ThrowTypeError(
			ctx, "audioPortToJS callback expects a value");
	mlregl::transport::audio::AudioCommandBatch audio;
	mlregl::transport::backend::BackendCommandBatch loads;
	std::vector<elm::AudioLoadRequest> requests;
	std::string error;
	if (!elm::audio_batch_from_js(ctx, argv[0], audio, loads, requests,
				      error)) {
		host->fail(std::move(error));
		return JS_ThrowTypeError(ctx, "%s", host->error_.c_str());
	}
	for (auto &request : requests)
		host->pending_audio_requests_[request.url].push_back(
			request.request_id);
	std::string bytes;
	if (loads.commands_size() > 0) {
		loads.SerializeToString(&bytes);
		host->enqueue_command(std::move(bytes), false);
	}
	if (audio.actions_size() > 0) {
		audio.SerializeToString(&bytes);
		host->audio_commands_.emplace_back(bytes.begin(), bytes.end());
	}
	return JS_UNDEFINED;
}

JSValue ElmHost::js_load_data_file(JSContext *ctx, JSValueConst, int argc,
				    JSValueConst *argv)
{
	auto *host = self(ctx);
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(
			ctx, "loadDataFile callback expects an object");
	JSValue name_value = JS_GetPropertyStr(ctx, argv[0], "name");
	JSValue path_value = JS_GetPropertyStr(ctx, argv[0], "path");
	const std::string name = js_string(ctx, name_value);
	const std::string path = js_string(ctx, path_value);
	const bool valid = JS_IsString(name_value) && JS_IsString(path_value);
	JS_FreeValue(ctx, name_value);
	JS_FreeValue(ctx, path_value);
	if (!valid)
		return JS_ThrowTypeError(
			ctx, "loadDataFile expects string name and path fields");
	host->pending_data_files_[path].push_back(name);
	mlregl::transport::backend::BackendCommandBatch batch;
	batch.add_commands()->mutable_load_file()->set_path(path);
	std::string bytes;
	batch.SerializeToString(&bytes);
	host->enqueue_command(std::move(bytes), false);
	return JS_UNDEFINED;
}

JSValue ElmHost::js_ignore(JSContext *, JSValueConst, int, JSValueConst *)
{
	return JS_UNDEFINED;
}

bool ElmHost::install_host_api()
{
	JSValue global = JS_GetGlobalObject(ctx_);
	JSValue host = JS_NewObject(ctx_);
	set_property(ctx_, host, "now",
		     JS_NewCFunction(ctx_, js_now, "now", 0));
	set_property(ctx_, host, "timeOrigin",
		     JS_NewCFunction(ctx_, js_time_origin, "timeOrigin", 0));
	set_property(ctx_, host, "setTimer",
		     JS_NewCFunction(ctx_, js_set_timer, "setTimer", 3));
	set_property(ctx_, host, "clearTimer",
		     JS_NewCFunction(ctx_, js_clear_timer, "clearTimer", 1));
	set_property(ctx_, host, "log",
		     JS_NewCFunction(ctx_, js_log, "log", 2));
	set_property(ctx_, global, "__declglHost", host);
	JS_FreeValue(ctx_, global);
	return eval_source(kHeadlessDomSource, sizeof(kHeadlessDomSource) - 1,
			   "declgl-headless-dom.js");
}

JSValue ElmHost::parse_flags()
{
	if (!config_.flags)
		return JS_UNDEFINED;
	auto text = read_file(*config_.flags);
	if (!text) {
		fail("cannot read flags file '" + config_.flags->string() +
		     "'");
		return JS_EXCEPTION;
	}
	JSValue value = JS_ParseJSON(ctx_, text->data(), text->size(),
				     config_.flags->string().c_str());
	if (JS_IsException(value))
		capture_exception("parse flags");
	return value;
}

JSValue ElmHost::get_port(const char *name, bool required)
{
	JSValue ports = JS_GetPropertyStr(ctx_, app_, "ports");
	JSValue port = JS_IsObject(ports) ?
			       JS_GetPropertyStr(ctx_, ports, name) :
			       JS_UNDEFINED;
	JS_FreeValue(ctx_, ports);
	if (required && !JS_IsObject(port))
		fail(std::string("missing required Elm port '") + name + "'");
	return port;
}

bool ElmHost::bind_ports()
{
	JSValue command_port = get_port("execREGLCmd", true);
	JSValue view_port = get_port("setView", true);
	update_port_ = get_port("reglupdate", true);
	recv_port_ = get_port("recvREGLCmd", true);
	JSValue audio_to_port = get_port("audioPortToJS", false);
	audio_from_port_ = get_port("audioPortFromJS", false);
	const bool has_audio_to = JS_IsObject(audio_to_port);
	const bool has_audio_from = JS_IsObject(audio_from_port_);
	if (has_audio_to != has_audio_from)
		fail("Elm audio requires both 'audioPortToJS' and "
		     "'audioPortFromJS' ports");
	if (failed_) {
		JS_FreeValue(ctx_, command_port);
		JS_FreeValue(ctx_, view_port);
		JS_FreeValue(ctx_, audio_to_port);
		return false;
	}
	auto subscribe_port = [&](JSValueConst port, JSCFunction *function,
				  const char *callback_name,
				  const char *operation) {
		JSValue subscribe = JS_GetPropertyStr(ctx_, port, "subscribe");
		JSValue callback =
			JS_NewCFunction(ctx_, function, callback_name, 1);
		JSValue result = JS_Call(ctx_, subscribe, port, 1, &callback);
		const bool exception = JS_IsException(result);
		JS_FreeValue(ctx_, result);
		JS_FreeValue(ctx_, callback);
		JS_FreeValue(ctx_, subscribe);
		if (exception)
			capture_exception(operation);
		return !exception;
	};
	JSValue subscribe = JS_GetPropertyStr(ctx_, command_port, "subscribe");
	JSValue callback =
		JS_NewCFunction(ctx_, js_command, "declglCommand", 1);
	JSValue result = JS_Call(ctx_, subscribe, command_port, 1, &callback);
	const bool command_exception = JS_IsException(result);
	JS_FreeValue(ctx_, result);
	JS_FreeValue(ctx_, callback);
	JS_FreeValue(ctx_, subscribe);
	if (command_exception) {
		JS_FreeValue(ctx_, command_port);
		JS_FreeValue(ctx_, view_port);
		JS_FreeValue(ctx_, audio_to_port);
		capture_exception("subscribe execREGLCmd");
		return false;
	}
	subscribe = JS_GetPropertyStr(ctx_, view_port, "subscribe");
	callback = JS_NewCFunction(ctx_, js_view, "declglView", 1);
	result = JS_Call(ctx_, subscribe, view_port, 1, &callback);
	const bool exception = JS_IsException(result);
	JS_FreeValue(ctx_, result);
	JS_FreeValue(ctx_, callback);
	JS_FreeValue(ctx_, subscribe);
	JS_FreeValue(ctx_, command_port);
	JS_FreeValue(ctx_, view_port);
	if (exception) {
		JS_FreeValue(ctx_, audio_to_port);
		capture_exception("subscribe setView");
		return false;
	}
	if (has_audio_to) {
		subscribe = JS_GetPropertyStr(ctx_, audio_to_port, "subscribe");
		callback = JS_NewCFunction(ctx_, js_audio, "declglAudio", 1);
		result = JS_Call(ctx_, subscribe, audio_to_port, 1, &callback);
		const bool audio_exception = JS_IsException(result);
		JS_FreeValue(ctx_, result);
		JS_FreeValue(ctx_, callback);
		JS_FreeValue(ctx_, subscribe);
		JS_FreeValue(ctx_, audio_to_port);
		if (audio_exception) {
			capture_exception("subscribe audioPortToJS");
			return false;
		}
	} else {
		JS_FreeValue(ctx_, audio_to_port);
	}

	JSValue load_data_port = get_port("loadDataFile", false);
	data_file_port_ = get_port("dataFileLoaded", false);
	const bool has_load_data = JS_IsObject(load_data_port);
	const bool has_data_file = JS_IsObject(data_file_port_);
	if (has_load_data != has_data_file) {
		JS_FreeValue(ctx_, load_data_port);
		fail("Elm data loading requires both 'loadDataFile' and "
		     "'dataFileLoaded' ports");
		return false;
	}
	if (has_load_data &&
	    !subscribe_port(load_data_port, js_load_data_file,
			    "declglLoadDataFile", "subscribe loadDataFile")) {
		JS_FreeValue(ctx_, load_data_port);
		return false;
	}
	JS_FreeValue(ctx_, load_data_port);

	for (const char *name : { "alert", "prompt", "sendInfo" }) {
		JSValue port = get_port(name, false);
		if (JS_IsObject(port) &&
		    !subscribe_port(port, js_ignore, "declglIgnore", name)) {
			JS_FreeValue(ctx_, port);
			return false;
		}
		JS_FreeValue(ctx_, port);
	}
	return true;
}

bool ElmHost::initialize()
{
	rt_ = JS_NewRuntime();
	if (!rt_) {
		fail("cannot create QuickJS runtime");
		return false;
	}
	JS_SetMemoryLimit(rt_, config_.memory_limit);
	JS_SetMaxStackSize(rt_, config_.stack_limit);
	JS_SetInterruptHandler(rt_, interrupt_handler, this);
	ctx_ = JS_NewContext(rt_);
	if (!ctx_) {
		fail("cannot create QuickJS context");
		return false;
	}
	JS_SetContextOpaque(ctx_, this);
	if (!install_host_api() || !eval_file(config_.script))
		return false;

	JSValue global = JS_GetGlobalObject(ctx_);
	JSValue current = JS_GetPropertyStr(ctx_, global, "Elm");
	JS_FreeValue(ctx_, global);
	std::size_t start = 0;
	while (start < config_.module.size() && JS_IsObject(current)) {
		const auto dot = config_.module.find('.', start);
		const std::string part =
			config_.module.substr(start, dot - start);
		JSValue next = JS_GetPropertyStr(ctx_, current, part.c_str());
		JS_FreeValue(ctx_, current);
		current = next;
		if (dot == std::string::npos)
			break;
		start = dot + 1;
	}
	if (!JS_IsObject(current)) {
		JS_FreeValue(ctx_, current);
		fail("Elm module '" + config_.module + "' was not found");
		return false;
	}
	JSValue init = JS_GetPropertyStr(ctx_, current, "init");
	if (!JS_IsFunction(ctx_, init)) {
		JS_FreeValue(ctx_, init);
		JS_FreeValue(ctx_, current);
		fail("Elm module has no init function");
		return false;
	}
	JSValue options = JS_NewObject(ctx_);
	JSValue global_root = JS_GetGlobalObject(ctx_);
	JSValue root = JS_GetPropertyStr(ctx_, global_root, "__declglRoot");
	JS_FreeValue(ctx_, global_root);
	set_property(ctx_, options, "node", root);
	JSValue flags = parse_flags();
	if (JS_IsException(flags)) {
		JS_FreeValue(ctx_, options);
		JS_FreeValue(ctx_, init);
		JS_FreeValue(ctx_, current);
		return false;
	}
	if (!JS_IsUndefined(flags))
		set_property(ctx_, options, "flags", flags);
	interrupt_deadline_ =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	app_ = JS_Call(ctx_, init, current, 1, &options);
	JS_FreeValue(ctx_, options);
	JS_FreeValue(ctx_, init);
	JS_FreeValue(ctx_, current);
	if (JS_IsException(app_)) {
		capture_exception("Elm init");
		return false;
	}
	if (!bind_ports())
		return false;
	JSValue global2 = JS_GetGlobalObject(ctx_);
	dispatch_fn_ = JS_GetPropertyStr(ctx_, global2, "__declglDispatch");
	JS_FreeValue(ctx_, global2);
	run_timers(false);
	return drain_jobs() && !failed_;
}

bool ElmHost::drain_jobs(std::size_t max_jobs)
{
	for (std::size_t i = 0; i < max_jobs; ++i) {
		JSContext *job_ctx = nullptr;
		interrupt_deadline_ = std::chrono::steady_clock::now() +
				      std::chrono::milliseconds(250);
		const int result = JS_ExecutePendingJob(rt_, &job_ctx);
		if (result == 0)
			return true;
		if (result < 0) {
			capture_exception("QuickJS job");
			return false;
		}
	}
	fail("QuickJS pending-job limit exceeded");
	return false;
}

void ElmHost::run_timers(bool include_animation_frame)
{
	const double now = now_ms();
	std::vector<Timer> due;
	for (auto it = timers_.begin(); it != timers_.end();) {
		if (it->due_ms <= now &&
		    (!it->animation_frame || include_animation_frame)) {
			due.push_back(*it);
			it = timers_.erase(it);
		} else
			++it;
	}
	for (auto &timer : due) {
		JSValue arg = JS_NewFloat64(ctx_, time_origin_ms_ + now);
		interrupt_deadline_ = std::chrono::steady_clock::now() +
				      std::chrono::milliseconds(250);
		JSValue result = JS_Call(ctx_, timer.callback, JS_UNDEFINED,
					 timer.animation_frame ? 1 : 0, &arg);
		JS_FreeValue(ctx_, arg);
		JS_FreeValue(ctx_, timer.callback);
		if (JS_IsException(result))
			capture_exception("timer callback");
		JS_FreeValue(ctx_, result);
		if (failed_)
			break;
	}
	drain_jobs();
}

std::vector<std::vector<uint8_t> > ElmHost::take_startup_commands()
{
	mlregl::transport::backend::BackendCommandBatch combined;
	while (!commands_.empty()) {
		mlregl::transport::backend::BackendCommandBatch batch;
		const auto &bytes = commands_.front();
		if (!batch.ParseFromArray(bytes.data(),
					  static_cast<int>(bytes.size()))) {
			fail("cannot decode queued Elm startup command");
			return {};
		}
		for (const auto &command : batch.commands())
			*combined.add_commands() = command;
		commands_.pop_front();
	}
	if (combined.commands_size() == 0)
		return {};
	std::string bytes;
	combined.SerializeToString(&bytes);
	return { std::vector<uint8_t>(bytes.begin(), bytes.end()) };
}

void ElmHost::enqueue_command(std::string bytes, bool start)
{
	std::vector<uint8_t> payload(bytes.begin(), bytes.end());
	if (!start_seen_ && !start) {
		pre_start_commands_.push_back(std::move(payload));
		return;
	}
	commands_.push_back(std::move(payload));
	if (!start || start_seen_)
		return;
	start_seen_ = true;
	while (!pre_start_commands_.empty()) {
		commands_.push_back(std::move(pre_start_commands_.front()));
		pre_start_commands_.pop_front();
	}
}

bool ElmHost::on_loop_enter()
{
	return !failed_;
}
void ElmHost::on_loop_exit()
{
}
void ElmHost::before_frame()
{
}
void ElmHost::before_events()
{
	if (!failed_)
		run_timers(true);
}
void ElmHost::before_view()
{
	if (!failed_) {
		run_timers(false);
		drain_jobs();
	}
}
void ElmHost::after_frame()
{
	++frame_count_;
	if (config_.max_frames > 0 && frame_count_ >= config_.max_frames)
		stop_requested_ = true;
}

std::vector<std::vector<uint8_t> > ElmHost::pull_commands()
{
	std::vector<std::vector<uint8_t> > out;
	while (!commands_.empty()) {
		out.push_back(std::move(commands_.front()));
		commands_.pop_front();
	}
	return out;
}

std::vector<std::vector<uint8_t> > ElmHost::pull_audio_commands()
{
	std::vector<std::vector<uint8_t> > out;
	while (!audio_commands_.empty()) {
		out.push_back(std::move(audio_commands_.front()));
		audio_commands_.pop_front();
	}
	return out;
}

bool ElmHost::send_port(JSValueConst port, JSValue value, const char *name)
{
	JSValue send = JS_GetPropertyStr(ctx_, port, "send");
	interrupt_deadline_ = std::chrono::steady_clock::now() +
			      std::chrono::milliseconds(250);
	JSValue result = JS_Call(ctx_, send, port, 1, &value);
	JS_FreeValue(ctx_, send);
	JS_FreeValue(ctx_, value);
	if (JS_IsException(result)) {
		JS_FreeValue(ctx_, result);
		capture_exception(name);
		return false;
	}
	JS_FreeValue(ctx_, result);
	return drain_jobs();
}

void ElmHost::dispatch_dom_event(const char *type, JSValue init)
{
	JSValue type_value = JS_NewString(ctx_, type);
	JSValue args[] = { type_value, init };
	interrupt_deadline_ = std::chrono::steady_clock::now() +
			      std::chrono::milliseconds(250);
	JSValue result = JS_Call(ctx_, dispatch_fn_, JS_UNDEFINED, 2, args);
	JS_FreeValue(ctx_, type_value);
	JS_FreeValue(ctx_, init);
	if (JS_IsException(result))
		capture_exception("DOM event");
	JS_FreeValue(ctx_, result);
	drain_jobs();
}

void ElmHost::deliver_event(const uint8_t *bytes, std::size_t len)
{
	if (failed_)
		return;
	mlregl::transport::backend::Event event;
	if (!event.ParseFromArray(bytes, static_cast<int>(len))) {
		fail("cannot decode native input event");
		return;
	}
	if (event.kind_case() ==
	    mlregl::transport::backend::Event::kUpdateTick) {
		send_port(update_port_,
			  JS_NewFloat64(ctx_, time_origin_ms_ +
						      event.update_tick().ts()),
			  "reglupdate.send");
		return;
	}
	std::string type, error;
	JSValue init = elm::input_event_to_js(ctx_, event, type, error);
	if (JS_IsException(init)) {
		fail(std::move(error));
		return;
	}
	dispatch_dom_event(type.c_str(), init);
}

std::optional<std::vector<uint8_t> > ElmHost::pull_view()
{
	return latest_view_;
}

void ElmHost::on_backend_event(const uint8_t *bytes, std::size_t len)
{
	if (failed_)
		return;
	mlregl::transport::backend::BackendEvent event;
	if (!event.ParseFromArray(bytes, static_cast<int>(len))) {
		fail("cannot decode native backend event");
		return;
	}
	using E = mlregl::transport::backend::BackendEvent;
	if (event.kind_case() == E::kFileLoaded ||
	    event.kind_case() == E::kFileLoadFailed) {
		if (!JS_IsObject(data_file_port_))
			return;
		const std::string &path = event.kind_case() == E::kFileLoaded ?
					  event.file_loaded().path() :
					  event.file_load_failed().path();
		auto it = pending_data_files_.find(path);
		if (it == pending_data_files_.end() || it->second.empty()) {
			fail("data file response for unrequested path '" + path +
			     "'");
			return;
		}
		const std::string name = std::move(it->second.front());
		it->second.pop_front();
		if (it->second.empty())
			pending_data_files_.erase(it);
		JSValue value = JS_NewObject(ctx_);
		set_property(ctx_, value, "name", JS_NewString(ctx_, name.c_str()));
		const std::string data = event.kind_case() == E::kFileLoaded ?
					 event.file_loaded().data() :
					 std::string();
		set_property(ctx_, value, "data",
			     JS_NewStringLen(ctx_, data.data(), data.size()));
		send_port(data_file_port_, value, "dataFileLoaded.send");
		return;
	}
	std::string error;
	JSValue value = elm::backend_event_to_js(ctx_, event, error);
	if (JS_IsException(value)) {
		fail(std::move(error));
		return;
	}
	send_port(recv_port_, value, "recvREGLCmd.send");
}

void ElmHost::on_audio_event(const uint8_t *bytes, std::size_t len)
{
	if (failed_ || !JS_IsObject(audio_from_port_))
		return;
	mlregl::transport::audio::AudioBackendEvent event;
	if (!event.ParseFromArray(bytes, static_cast<int>(len))) {
		fail("cannot decode native audio event");
		return;
	}
	std::optional<int64_t> request_id;
	std::string url;
	using E = mlregl::transport::audio::AudioBackendEvent;
	if (event.kind_case() == E::kAudioLoadSuccess)
		url = event.audio_load_success().audio_url();
	else if (event.kind_case() == E::kAudioLoadFailed)
		url = event.audio_load_failed().audio_url();
	if (!url.empty()) {
		auto it = pending_audio_requests_.find(url);
		if (it != pending_audio_requests_.end() && !it->second.empty()) {
			request_id = it->second.front();
			it->second.pop_front();
			if (it->second.empty())
				pending_audio_requests_.erase(it);
		}
	}
	std::string error;
	JSValue value = elm::audio_event_to_js(ctx_, event, request_id, error);
	if (JS_IsException(value)) {
		fail(std::move(error));
		return;
	}
	send_port(audio_from_port_, value, "audioPortFromJS.send");
}

} // namespace declgl
