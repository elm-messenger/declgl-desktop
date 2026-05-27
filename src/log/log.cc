#include "log/log.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/core/PatternFormatterOptions.h>
#include <quill/sinks/ConsoleSink.h>

namespace declgl
{
namespace log
{

namespace
{

std::atomic<bool> &init_once_flag()
{
	static std::atomic<bool> f{ false };
	return f;
}

quill::Logger *&the_logger()
{
	static quill::Logger *l = nullptr;
	return l;
}

quill::LogLevel parse_log_level(const char *s)
{
	if (!s || !*s) {
		return quill::LogLevel::Warning;
	}
	std::string v(s);
	// case-insensitive compare
	for (auto &c : v) {
		c = static_cast<char>(
			std::tolower(static_cast<unsigned char>(c)));
	}
	if (v == "trace" || v == "trace3" || v == "tracel3")
		return quill::LogLevel::TraceL3;
	if (v == "trace2" || v == "tracel2")
		return quill::LogLevel::TraceL2;
	if (v == "trace1" || v == "tracel1")
		return quill::LogLevel::TraceL1;
	if (v == "debug")
		return quill::LogLevel::Debug;
	if (v == "info")
		return quill::LogLevel::Info;
	if (v == "notice")
		return quill::LogLevel::Notice;
	if (v == "warning" || v == "warn")
		return quill::LogLevel::Warning;
	if (v == "error" || v == "err")
		return quill::LogLevel::Error;
	if (v == "critical" || v == "fatal")
		return quill::LogLevel::Critical;
	return quill::LogLevel::Warning;
}

} // namespace

void init()
{
	bool expected = false;
	if (!init_once_flag().compare_exchange_strong(expected, true)) {
		return;
	}

	quill::BackendOptions backend_options;
	quill::Backend::start(backend_options);

	auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
		"declgl_console");

	auto *q = quill::Frontend::create_or_get_logger("declgl",
							std::move(sink));

	// Read log level from environment; default to Warning.
	const char *env_level = std::getenv("DECLGL_LOG_LEVEL");
	quill::LogLevel level = parse_log_level(env_level);
	q->set_log_level(level);

	the_logger() = q;
}

void shutdown()
{
	if (!init_once_flag().load()) {
		return;
	}
	quill::Backend::stop();
	the_logger() = nullptr;
	init_once_flag().store(false);
}

quill::Logger *logger()
{
	if (!init_once_flag().load()) {
		init();
	}
	return the_logger();
}

} // namespace log
} // namespace declgl
