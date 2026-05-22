#include "log/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/core/PatternFormatterOptions.h>
#include <quill/sinks/ConsoleSink.h>

namespace declgl {
namespace log {

// LoggerHandle is just a thin alias to quill::Logger so the public
// header can keep quill out of its includes. The reinterpret_cast on
// the boundary is a no-op at runtime.
struct LoggerHandle {
	quill::Logger *q;
};

namespace {

std::atomic<bool> &init_once_flag()
{
	static std::atomic<bool> f{false};
	return f;
}

std::mutex &registry_mutex()
{
	static std::mutex m;
	return m;
}

std::unordered_map<std::string, LoggerHandle *> &registry()
{
	static std::unordered_map<std::string, LoggerHandle *> m;
	return m;
}

// Pattern without %(short_source_location). Keeps timestamp, thread id,
// log level, logger name, and the message itself.
quill::PatternFormatterOptions pattern_options()
{
	return quill::PatternFormatterOptions{
		"%(time) [%(thread_id)] LOG_%(log_level:<9) "
		"%(logger:<24) %(message)"};
}

LoggerHandle *make_handle(quill::Logger *q)
{
	auto *h = new LoggerHandle{q};
	return h;
}

// vsnprintf-into-stack-buffer; falls back to a heap buffer for very
// long messages (> 2 KiB). Returns the formatted string.
std::string format_v(const char *fmt, std::va_list ap)
{
	char stack_buf[2048];
	std::va_list ap2;
	va_copy(ap2, ap);
	int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
	if (n < 0) {
		va_end(ap2);
		return std::string("<format error>");
	}
	if (static_cast<size_t>(n) < sizeof(stack_buf)) {
		va_end(ap2);
		return std::string(stack_buf, static_cast<size_t>(n));
	}
	std::string out(static_cast<size_t>(n) + 1, '\0');
	std::vsnprintf(out.data(), out.size(), fmt, ap2);
	va_end(ap2);
	out.resize(static_cast<size_t>(n));
	return out;
}

} // namespace

void init()
{
	bool expected = false;
	if (!init_once_flag().compare_exchange_strong(expected, true)) {
		return;
	}

	// Backend thread: handles formatting + I/O. quill::BackendOptions
	// defaults are sensible for a desktop game (1 thread, no CPU pin,
	// 8 KiB ring buffer per frontend thread).
	quill::BackendOptions backend_options;
	quill::Backend::start(backend_options);

	// Single console sink shared by every logger. Color enabled when
	// the terminal supports it (quill detects).
	auto sink =
		quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
			"declgl_console");

	auto *q = quill::Frontend::create_or_get_logger(
		"declgl", std::move(sink), pattern_options());

	// Default level: Info. The plan TODO ("env var to set logging
	// level") will plug into this here when implemented.
	q->set_log_level(quill::LogLevel::Info);

	{
		std::lock_guard<std::mutex> lk(registry_mutex());
		registry()["declgl"] = make_handle(q);
	}
}

void shutdown()
{
	if (!init_once_flag().load()) {
		return;
	}
	// Drain pending log records before tearing down sinks.
	quill::Backend::stop();
	{
		std::lock_guard<std::mutex> lk(registry_mutex());
		for (auto &kv : registry())
			delete kv.second;
		registry().clear();
	}
	init_once_flag().store(false);
}

LoggerHandle *logger_for(const char *category)
{
	if (!category || !*category) {
		category = "declgl";
	}
	{
		std::lock_guard<std::mutex> lk(registry_mutex());
		auto &reg = registry();
		auto it = reg.find(category);
		if (it != reg.end())
			return it->second;
	}
	// Lazy-create a child logger sharing the root sink. Levels can
	// later be tuned per category (env var TODO).
	if (!init_once_flag().load()) {
		// Caller logged before init(). Bring quill up so we don't
		// drop the message; this is a defensive path — engine
		// startup should have called init() already.
		init();
	}
	auto sink = quill::Frontend::get_sink("declgl_console");
	auto *q = quill::Frontend::create_or_get_logger(
		category, std::move(sink), pattern_options());
	q->set_log_level(quill::LogLevel::Info);
	auto *h = make_handle(q);
	{
		std::lock_guard<std::mutex> lk(registry_mutex());
		auto [it, inserted] = registry().emplace(category, h);
		if (!inserted) {
			// Lost a race with another thread; drop ours and
			// return the registered one.
			delete h;
			return it->second;
		}
	}
	return h;
}

void log_info_v(LoggerHandle *logger, const char *fmt, std::va_list ap)
{
	if (!logger || !logger->q)
		return;
	const std::string msg = format_v(fmt, ap);
	LOG_INFO(logger->q, "{}", msg);
}

void log_warn_v(LoggerHandle *logger, const char *fmt, std::va_list ap)
{
	if (!logger || !logger->q)
		return;
	const std::string msg = format_v(fmt, ap);
	LOG_WARNING(logger->q, "{}", msg);
}

void log_error_v(LoggerHandle *logger, const char *fmt, std::va_list ap)
{
	if (!logger || !logger->q)
		return;
	const std::string msg = format_v(fmt, ap);
	LOG_ERROR(logger->q, "{}", msg);
}

} // namespace log
} // namespace declgl
