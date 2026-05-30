// runtime.h — transport-agnostic SDL/GL runtime for the desktop ml_regl
// backend.
//
// Owns the SDL window + GL context, the per-frame loop, the
// BackendCommand/BackendEvent dispatch, frame pacing, profiling, and the
// process-wide engine instance. Everything host-specific (OCaml FFI vs.
// gRPC player vs. test harness) is funnelled through the LoopHooks
// abstract base class (see loop_hooks.h).
//
// Lifetime, mirroring the previous in-bridge topology:
//
//   1. Construct Runtime with a host-supplied LoopHooks. The runtime
//      lazily creates a declgl::Engine and registers the engine sinks
//      that re-emit BackendEvents / AudioBackendEvents through the
//      host's hooks.
//   2. The host calls dispatch(bytes, len) for every inbound
//      BackendCommandBatch. The runtime decodes and dispatches inline,
//      same convention as the JS backend's synchronous loop.
//   3. The first StartRegl in a batch brings up SDL + window + GL ctx
//      and arms the run loop. dispatch() returns true; the host should
//      then call run() to enter the per-frame loop.
//   4. run() returns when the user closes the window, ESC is pressed,
//      or a QuitRegl is observed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mlregl::transport::backend
{
class BackendCommand;
class BackendCommandBatch;
} // namespace mlregl::transport::backend

namespace declgl
{

class Engine;
class LoopHooks;

class Runtime
{
    public:
	// Captures `hooks` by reference; the caller owns the LoopHooks
	// instance and must keep it alive for the lifetime of Runtime.
	explicit Runtime(LoopHooks &hooks);
	~Runtime();
	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	// Decode and dispatch a BackendCommandBatch payload. Re-entrant
	// from inside hooks (e.g. a host's pull_view callback that ships
	// further commands). May internally bring up SDL + window + GL on
	// first StartRegl. Returns true iff the batch contained a
	// StartRegl AND init_window_and_gl succeeded — the caller should
	// then call run() to enter the per-frame loop.
	bool dispatch(const uint8_t *bytes, std::size_t len);

	// Decode and dispatch an AudioCommandBatch payload through the
	// engine. Stamps now_ms relative to the runtime's start anchor
	// (zero before the loop has started).
	void dispatch_audio(const uint8_t *bytes, std::size_t len);

	// Per-frame loop. Returns when SDL_QUIT / ESC / QuitRegl is seen.
	// Precondition: a prior dispatch() call must have surfaced a
	// StartRegl that brought the GL context up.
	void run();

	// True while run() is executing (between on_loop_enter and
	// on_loop_exit). Visible to hosts that need a re-entrancy guard
	// before calling run() again.
	bool is_loop_running() const;

	// Direct accessor; useful for tests. May be nullptr until the
	// first dispatch() / dispatch_audio() call lazily constructs the
	// engine.
	Engine *engine();

    private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace declgl
