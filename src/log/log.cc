#include "log/log.h"

#include <atomic>

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
	q->set_log_level(quill::LogLevel::Info);

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
