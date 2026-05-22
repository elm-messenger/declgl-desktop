#pragma once

// declgl::log — thin wrapper over quill, with a printf-compatible API
// so the existing call sites (which use std::printf / std::fprintf with
// %s, %d, %g, ...) can be converted mechanically.
//
// Why not use quill's native fmt-style ({}, {:.3f}, ...) directly? Two
// reasons:
//
//   1. There are 70+ call sites in this codebase already shaped around
//      printf format strings. Rewriting each one to fmt syntax is risky
//      churn for no immediate user-visible win.
//
//   2. Per-frame call sites (e.g. the per-command dispatch logs) are
//      not actually on the GPU-bound critical path here — a vsnprintf
//      into a small stack buffer is a few hundred ns and well below
//      the per-frame budget. We retain quill's async I/O (the backend
//      thread does the actual write), which is the part that *would*
//      hurt in the hot path under sync printf.
//
// Categories: each logical subsystem ("declgl", "declgl/bridge",
// "declgl/audio", ...) has its own quill::Logger* so the level can be
// tuned per subsystem at runtime via env (DECLGL_LOG_LEVEL=...; later
// per-category overrides). The lookup is a single atomic load on the
// hot path — see [logger_for] below.
//
// API:
//
//   declgl::log::info  ("declgl",        "GL %d.%d ready", maj, min);
//   declgl::log::warn  ("declgl/bridge", "duplicate StartRegl ignored");
//   declgl::log::error ("declgl/audio",  "decode failed: %s", err);
//
// init() / shutdown() bracket quill's backend thread; call them once
// at process start / end (Engine does this in [init_decoders_only] /
// [shutdown]).

#include <cstdarg>
#include <cstdio>

namespace quill {
template <typename> class LoggerImpl;
struct FrontendOptions;
} // namespace quill

namespace declgl {
namespace log {

// Bring up the quill backend thread + the default console sink. Idempotent.
void init();

// Stop the backend thread; flushes pending records first. Idempotent.
void shutdown();

// Opaque logger handle. The .cc has the full quill type; callers only
// pass these around.
struct LoggerHandle;

// Get-or-create a logger for [category]. Cheap on the hot path: each
// distinct string maps to one cached logger. Pass the same string each
// call site; we avoid an std::string round-trip by keying on the
// const char*.
LoggerHandle *logger_for(const char *category);

// printf-style entry points. The actual quill macro expansion happens
// inside the .cc, so headers including this file don't pull in the
// full quill headers.
void log_info_v(LoggerHandle *logger, const char *fmt, std::va_list ap);
void log_warn_v(LoggerHandle *logger, const char *fmt, std::va_list ap);
void log_error_v(LoggerHandle *logger, const char *fmt, std::va_list ap);

inline void info(const char *cat, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
inline void warn(const char *cat, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
inline void error(const char *cat, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

inline void info(const char *cat, const char *fmt, ...)
{
	std::va_list ap;
	va_start(ap, fmt);
	log_info_v(logger_for(cat), fmt, ap);
	va_end(ap);
}
inline void warn(const char *cat, const char *fmt, ...)
{
	std::va_list ap;
	va_start(ap, fmt);
	log_warn_v(logger_for(cat), fmt, ap);
	va_end(ap);
}
inline void error(const char *cat, const char *fmt, ...)
{
	std::va_list ap;
	va_start(ap, fmt);
	log_error_v(logger_for(cat), fmt, ap);
	va_end(ap);
}

} // namespace log
} // namespace declgl
