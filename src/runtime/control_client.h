#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace declgl
{

// Small, optional JSON-over-WebSocket control transport. The websocket
// callback never touches SDL, OpenGL, OCaml, or Runtime state: it only queues
// text for the render thread to consume at a frame boundary.
class ControlClient {
    public:
	ControlClient() = default;
	~ControlClient();
	ControlClient(const ControlClient &) = delete;
	ControlClient &operator=(const ControlClient &) = delete;

	void start_from_environment();
	void stop();
	bool enabled() const;

	std::vector<std::string> poll_commands();
	void send_json(const nlohmann::json &message);
	void emit_debug_line(std::string_view line);

	std::string latest_state() const;
	std::vector<std::string> recent_logs() const;

    private:
	struct Impl;
	Impl *impl_ = nullptr;
};

ControlClient &control_client();

// Called by the OCaml debug sink. It is intentionally safe before the client
// has connected; diagnostics remain useful on stdout in that case.
void declgl_debug_emit_line(const char *line, std::size_t length);

} // namespace declgl
