#pragma once

#include <quickjs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/loop_hooks.h"

namespace declgl
{

struct ElmHostConfig {
	std::filesystem::path script;
	std::string module;
	std::string app_name;
	bool fullscreen = false;
	std::size_t memory_limit = 128 * 1024 * 1024;
	std::size_t stack_limit = 2 * 1024 * 1024;
	std::size_t max_frames = 0;
};

class ElmHost final : public LoopHooks {
    public:
	explicit ElmHost(ElmHostConfig config);
	~ElmHost() override;
	ElmHost(const ElmHost &) = delete;
	ElmHost &operator=(const ElmHost &) = delete;

	bool initialize();
	bool ok() const
	{
		return !failed_;
	}
	const std::string &error() const
	{
		return error_;
	}
	std::vector<std::vector<uint8_t> > take_startup_commands();

	bool on_loop_enter() override;
	void on_loop_exit() override;
	void before_frame() override;
	void before_events() override;
	void before_view() override;
	void after_frame() override;
	std::vector<std::vector<uint8_t> > pull_commands() override;
	std::vector<std::vector<uint8_t> > pull_audio_commands() override;
	void deliver_event(const uint8_t *bytes, std::size_t len) override;
	std::optional<std::vector<uint8_t> > pull_view() override;
	void on_backend_event(const uint8_t *bytes, std::size_t len) override;
	void on_audio_event(const uint8_t *, std::size_t) override;
	bool should_continue() const override
	{
		return !failed_ && !stop_requested_;
	}

    private:
	struct Timer {
		uint64_t id;
		double due_ms;
		bool animation_frame;
		JSValue callback;
	};

	ElmHostConfig config_;
	JSRuntime *rt_ = nullptr;
	JSContext *ctx_ = nullptr;
	JSValue app_ = JS_UNDEFINED;
	JSValue update_port_ = JS_UNDEFINED;
	JSValue recv_port_ = JS_UNDEFINED;
	JSValue audio_from_port_ = JS_UNDEFINED;
	JSValue data_file_port_ = JS_UNDEFINED;
	JSValue dispatch_fn_ = JS_UNDEFINED;
	JSValue resize_fn_ = JS_UNDEFINED;
	std::deque<std::vector<uint8_t> > commands_;
	std::deque<std::vector<uint8_t> > pre_start_commands_;
	std::deque<std::vector<uint8_t> > audio_commands_;
	std::unordered_map<std::string, std::deque<int64_t> >
		pending_audio_requests_;
	std::unordered_map<std::string, std::deque<std::string> >
		pending_data_files_;
	std::optional<std::vector<uint8_t> > latest_view_;
	std::vector<Timer> timers_;
	uint64_t next_timer_id_ = 1;
	std::chrono::steady_clock::time_point started_;
	double time_origin_ms_ = 0.0;
	bool failed_ = false;
	bool start_seen_ = false;
	bool viewport_resize_pending_ = false;
	bool stop_requested_ = false;
	std::size_t frame_count_ = 0;
	std::string error_;
	std::chrono::steady_clock::time_point interrupt_deadline_;

	bool eval_file(const std::filesystem::path &path);
	bool eval_source(const char *source, std::size_t len, const char *name);
	bool install_host_api();
	bool bind_ports();
	bool drain_jobs(std::size_t max_jobs = 10000);
	void run_timers(bool include_animation_frame);
	void fail(std::string message);
	void capture_exception(const char *operation);
	double now_ms() const;
	JSValue parse_flags();
	JSValue get_port(const char *name, bool required);
	bool send_port(JSValueConst port, JSValue value, const char *name);
	void dispatch_dom_event(const char *type, JSValue init);
	void set_dom_viewport(double width, double height);
	void enqueue_command(std::string bytes, bool start);

	static ElmHost *self(JSContext *ctx);
	static int interrupt_handler(JSRuntime *, void *opaque);
	static JSValue js_now(JSContext *, JSValueConst, int, JSValueConst *);
	static JSValue js_time_origin(JSContext *, JSValueConst, int,
				      JSValueConst *);
	static JSValue js_set_timer(JSContext *, JSValueConst, int,
				    JSValueConst *);
	static JSValue js_clear_timer(JSContext *, JSValueConst, int,
				      JSValueConst *);
	static JSValue js_log(JSContext *, JSValueConst, int, JSValueConst *);
	static JSValue js_command(JSContext *, JSValueConst, int,
				  JSValueConst *);
	static JSValue js_view(JSContext *, JSValueConst, int, JSValueConst *);
	static JSValue js_audio(JSContext *, JSValueConst, int, JSValueConst *);
	static JSValue js_load_data_file(JSContext *, JSValueConst, int,
				     JSValueConst *);
	static JSValue js_save_info(JSContext *, JSValueConst, int,
				JSValueConst *);
	static JSValue js_ignore(JSContext *, JSValueConst, int, JSValueConst *);
};

} // namespace declgl
