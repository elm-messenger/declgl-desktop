#include "runtime/control_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

namespace declgl
{

using json = nlohmann::json;

struct ControlClient::Impl {
	ix::WebSocket socket;
	mutable std::mutex mutex;
	std::vector<std::string> commands;
	std::string latest_state_json;
	std::vector<std::string> logs;
	bool enabled = false;
	bool started = false;
};

namespace
{

bool env_true(const char *name)
{
	const char *value = std::getenv(name);
	if (!value)
		return false;
	std::string v(value);
	std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

ControlClient::~ControlClient()
{
	stop();
	delete impl_;
}

void ControlClient::start_from_environment()
{
	if (!impl_)
		impl_ = new Impl;
	if (impl_->started)
		return;
	const char *url = std::getenv("DECLGL_CONTROL_URL");
	if (!url || !*url || (!env_true("DECLGL_DEBUG") &&
				      !env_true("DECLGL_REMOTE_CONTROL")))
		return;

	impl_->enabled = true;
	impl_->socket.setUrl(url);
	impl_->socket.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
		if (msg->type == ix::WebSocketMessageType::Open) {
			json hello = { { "type", "hello" },
				       { "protocol", 1 },
				       { "runtime", "ml-regl-desktop" },
				       { "capabilities", { "pause", "resume", "quit", "step",
									"set_time", "get_state", "get_render_tree",
									"screenshot", "input" } } };
			impl_->socket.send(hello.dump());
		} else if (msg->type == ix::WebSocketMessageType::Message) {
			if (msg->str.size() > 1024 * 1024)
				return;
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->commands.push_back(msg->str);
			if (impl_->commands.size() > 256)
				impl_->commands.erase(impl_->commands.begin());
		}
	});
	impl_->started = true;
	impl_->socket.start();
}

void ControlClient::stop()
{
	if (!impl_ || !impl_->started)
		return;
	impl_->socket.stop();
	impl_->started = false;
}

bool ControlClient::enabled() const
{
	return impl_ && impl_->enabled && impl_->started;
}

std::vector<std::string> ControlClient::poll_commands()
{
	if (!impl_)
		return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<std::string> result;
	result.swap(impl_->commands);
	return result;
}

void ControlClient::send_json(const json &message)
{
	if (!enabled())
		return;
	impl_->socket.send(message.dump());
}

void ControlClient::emit_debug_line(std::string_view line)
{
	if (line.empty())
		return;
	if (!impl_)
		impl_ = new Impl;
	std::string normalized(line);
	while (!normalized.empty() &&
	       (normalized.back() == '\n' || normalized.back() == '\r'))
		normalized.pop_back();
	if (normalized.empty())
		return;
	// Keep the existing stdout contract for local diagnostics.
	std::fwrite(normalized.data(), 1, normalized.size(), stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);

	if (normalized.rfind("MCP_STATE ", 0) == 0) {
		const std::string payload(normalized.substr(10));
		if (impl_) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->latest_state_json = payload;
		}
		json event = { { "type", "state" } };
		try {
			event["state"] = json::parse(payload);
		} catch (...) {
			event["state"] = payload;
		}
		send_json(event);
		return;
	}

	if (normalized.rfind("MCP_LOG ", 0) == 0) {
		const std::string payload(normalized.substr(8));
		std::string level = "info";
		std::string message = payload;
		const auto split = payload.find(' ');
		if (split != std::string::npos) {
			level = payload.substr(0, split);
			message = payload.substr(split + 1);
		}
		if (impl_) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->logs.push_back(message);
			if (impl_->logs.size() > 64)
				impl_->logs.erase(impl_->logs.begin());
		}
		send_json({ { "type", "log" }, { "level", level },
				    { "message", message } });
	}
}

std::string ControlClient::latest_state() const
{
	if (!impl_)
		return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->latest_state_json;
}

std::vector<std::string> ControlClient::recent_logs() const
{
	if (!impl_)
		return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->logs;
}

ControlClient &control_client()
{
	static ControlClient client;
	return client;
}

void declgl_debug_emit_line(const char *line, std::size_t length)
{
	if (!line)
		return;
	control_client().emit_debug_line(std::string_view(line, length));
}

} // namespace declgl
