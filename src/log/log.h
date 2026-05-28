#pragma once

// declgl::log — single shared quill logger with native fmt-style formatting.
//
// Usage:
//   #include "log/log.h"
//   DECLGL_LOG_INFO("GL {}.{} ready", maj, min);
//   DECLGL_LOG_WARN("duplicate StartRegl ignored");
//   DECLGL_LOG_ERROR("decode failed: {}", err);
//
// init() / shutdown() bracket quill's backend thread; Engine calls them.

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/bundled/fmt/format.h>

namespace declgl
{
namespace log
{

// Bring up the quill backend thread + console sink. Idempotent.
void init();

// Stop the backend thread; flushes pending records first. Idempotent.
void shutdown();

// Returns the single shared logger. Used by the macros below;
// callers normally use DECLGL_LOG_*() directly.
quill::Logger *logger();

// Thin macros over quill's LOG_* that supply the shared logger.
// Format strings use quill's native fmt syntax ("{}" not "%d").
// Level order (lowest → highest): TRACE < DEBUG < INFO < WARN < ERROR.
// Set DECLGL_LOG_LEVEL=trace (or =debug) at runtime to see them.
#define DECLGL_LOG_TRACE(...)                                                  \
	LOG_TRACE_L1(::declgl::log::logger(), __VA_ARGS__)
#define DECLGL_LOG_DEBUG(...) LOG_DEBUG(::declgl::log::logger(), __VA_ARGS__)
#define DECLGL_LOG_INFO(...) LOG_INFO(::declgl::log::logger(), __VA_ARGS__)
#define DECLGL_LOG_WARN(...) LOG_WARNING(::declgl::log::logger(), __VA_ARGS__)
#define DECLGL_LOG_ERROR(...) LOG_ERROR(::declgl::log::logger(), __VA_ARGS__)

} // namespace log
} // namespace declgl
