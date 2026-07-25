// loop_hooks.h — abstract boundary between the desktop runtime and its host.
//
// The runtime (see runtime.h) owns the SDL/GL window and the per-frame loop;
// it knows nothing about its caller (OCaml FFI bridge, gRPC player, unit
// test harness). Everything that crosses the boundary goes through the
// pure-virtual hook methods below. Each transport implements LoopHooks once
// and hands the instance to a Runtime.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace declgl
{

class LoopHooks {
    public:
	virtual ~LoopHooks() = default;

	// --- Lifecycle --------------------------------------------------
	// Called once after init_window_and_gl succeeds, before the first
	// frame. Returning false aborts startup (the run loop never enters).
	virtual bool on_loop_enter()
	{
		return true;
	}

	// Called once after the last frame, before engine->shutdown().
	virtual void on_loop_exit()
	{
	}

	// Invoked when a QuitRegl command is observed. The runtime has
	// already set its internal quit flag; this is purely a notification
	// for the host (e.g. so a gRPC player can half-close its stream).
	virtual void on_quit()
	{
	}

	// --- Per-frame fences ------------------------------------------
	// Called at the top of each frame, before pump_events / view.
	virtual void before_frame()
	{
	}
	// Called immediately before SDL input is pumped. Hosts with an embedded
	// scheduler can run frame callbacks here.
	virtual void before_events()
	{
	}
	// Called after SDL events and UpdateTick, before the view is pulled.
	// Hosts may finish language-runtime jobs here; any commands they queued
	// are drained immediately afterwards.
	virtual void before_view()
	{
	}
	// Called at the very end of each frame, after swap + pacing sleep.
	virtual void after_frame()
	{
	}
	virtual bool should_continue() const
	{
		return true;
	}

	// --- Command intake --------------------------------------------
	// Called once per frame inside before_frame's slot. Implementations
	// return any inbound BackendCommandBatch payloads (each entry a
	// serialized BackendCommandBatch) that have arrived since the last
	// call. Empty vector = nothing to dispatch this frame. The runtime
	// parses each blob and routes it through its dispatch path.
	virtual std::vector<std::vector<uint8_t> > pull_commands()
	{
		return {};
	}
	virtual std::vector<std::vector<uint8_t> > pull_audio_commands()
	{
		return {};
	}

	// --- Event egress (input events to the host) -------------------
	// Called for every SDL input event AND for the per-frame
	// UpdateTick. `bytes` is a serialized
	// mlregl::transport::backend::Event payload.
	virtual void deliver_event(const uint8_t *bytes, std::size_t len) = 0;

	// --- View pull (render tree) -----------------------------------
	// Returns serialized Renderable bytes when the host wants a frame
	// rendered, std::nullopt to skip render this frame. Empty vector
	// is also acceptable and means "render nothing".
	virtual std::optional<std::vector<uint8_t> > pull_view() = 0;

	// --- BackendEvent / AudioBackendEvent egress -------------------
	// Wired into engine->set_event_sink / set_audio_event_sink at
	// runtime construction.
	virtual void on_backend_event(const uint8_t *bytes,
				      std::size_t len) = 0;
	virtual void on_audio_event(const uint8_t *bytes, std::size_t len) = 0;
};

} // namespace declgl
