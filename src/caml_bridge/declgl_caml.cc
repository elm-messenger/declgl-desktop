// declgl_caml.cc — OCaml↔C++ bridge for the desktop ml_regl backend.
//
// This file provides the C symbols that [lib/backend/desktop/regl_desktop.ml]
// declares as `external`:
//
//     external declgl_ship_backend_cmd : bytes -> unit
//     external declgl_ship_audio_cmd   : bytes -> unit
//
// There is intentionally NO separate `declgl_run_main` entry point. The
// run loop is triggered by the protocol-level [StartRegl] command — the
// same way the JS backend's `start()` is what schedules
// `requestAnimationFrame(step)`. So the topology is:
//
//   1. OCaml owns process & main thread (provided by the OCaml runtime's
//      C `main`).
//   2. User OCaml code's top-level evaluates
//          let _ = Regl_desktop.create_app init update view
//      which (i) Callback.registers the runtime closures and (ii) calls
//      [h.init ()], whose first action is to ship a BackendCommandBatch
//      that contains a [StartRegl] entry.
//   3. [declgl_ship_backend_cmd] hands the bytes to the runtime, which
//      dispatches every command in order. After dispatch, if a StartRegl
//      brought up the window, this function calls runtime.run() which
//      blocks until the user closes the window or QuitRegl is observed.
//   4. Each frame the runtime calls back into OCaml via the
//      [CamlLoopHooks] adapter for every pending event, then for [view].
//      All callbacks run on the same OCaml-runtime thread, so no
//      acquire/release dance is required.

#define CAML_NAME_SPACE
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/memory.h>
#include <caml/mlvalues.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "log/log.h"
#include "runtime/loop_hooks.h"
#include "runtime/runtime.h"

namespace
{

// LoopHooks subclass that bridges every runtime callback into an OCaml
// closure registered via [Callback.register]. Callback name pointers are
// resolved lazily because user-side [Callback.register]s may not have
// run by the time the bridge constructs its singleton.
class CamlLoopHooks final : public declgl::LoopHooks {
    public:
	bool on_loop_enter() override
	{
		event_cb_ = caml_named_value("declgl_app_event");
		view_cb_ = caml_named_value("declgl_app_view");
		if (!event_cb_ || !view_cb_) {
			DECLGL_LOG_ERROR(
				"missing required Callback.register: "
				"event={} view={}",
				(void *)event_cb_, (void *)view_cb_);
			return false;
		}
		return true;
	}

	void deliver_event(const uint8_t *bytes, std::size_t len) override
	{
		if (!event_cb_)
			return;
		CAMLparam0();
		CAMLlocal1(v_bytes);
		v_bytes = caml_alloc_initialized_string(
			len, reinterpret_cast<const char *>(bytes));
		caml_callback(*event_cb_, v_bytes);
		CAMLdrop;
	}

	std::optional<std::vector<uint8_t> > pull_view() override
	{
		if (!view_cb_)
			return std::nullopt;
		CAMLparam0();
		CAMLlocal1(v_view);
		v_view = caml_callback(*view_cb_, Val_unit);
		if (!Is_block(v_view)) {
			CAMLdrop;
			return std::nullopt;
		}
		// Some bytes
		value v_bytes = Field(v_view, 0);
		const uint8_t *data =
			reinterpret_cast<const uint8_t *>(Bytes_val(v_bytes));
		const std::size_t len = caml_string_length(v_bytes);
		std::vector<uint8_t> out(data, data + len);
		CAMLdrop;
		return out;
	}

	void on_backend_event(const uint8_t *bytes, std::size_t len) override
	{
		const value *recv =
			caml_named_value("declgl_app_recv_regl_cmd_pb");
		if (!recv) {
			DECLGL_LOG_ERROR(
				"BackendEvent dropped: callback "
				"'declgl_app_recv_regl_cmd_pb' not registered");
			return;
		}
		CAMLparam0();
		CAMLlocal1(v_bytes);
		v_bytes = caml_alloc_initialized_string(
			len, reinterpret_cast<const char *>(bytes));
		caml_callback(*recv, v_bytes);
		CAMLdrop;
	}

	void on_audio_event(const uint8_t *bytes, std::size_t len) override
	{
		const value *recv =
			caml_named_value("declgl_app_recv_audio_msg_pb");
		if (!recv) {
			DECLGL_LOG_ERROR(
				"AudioBackendEvent dropped: callback "
				"'declgl_app_recv_audio_msg_pb' not registered");
			return;
		}
		CAMLparam0();
		CAMLlocal1(v_bytes);
		v_bytes = caml_alloc_initialized_string(
			len, reinterpret_cast<const char *>(bytes));
		caml_callback(*recv, v_bytes);
		CAMLdrop;
	}

    private:
	const value *event_cb_ = nullptr;
	const value *view_cb_ = nullptr;
};

// Process-wide singletons. The hooks must outlive the runtime; both are
// function-local statics with the conventional initialization order.
CamlLoopHooks &caml_hooks()
{
	static CamlLoopHooks h;
	return h;
}

declgl::Runtime &runtime()
{
	static declgl::Runtime r(caml_hooks());
	return r;
}

} // namespace

// ---------------------------------------------------------------------------
// OCaml-callable primitives
// ---------------------------------------------------------------------------

extern "C" CAMLprim value declgl_ship_backend_cmd(value v_bytes)
{
	CAMLparam1(v_bytes);

	auto &rt = runtime();

	const uint8_t *p =
		reinterpret_cast<const uint8_t *>(Bytes_val(v_bytes));
	const size_t n = caml_string_length(v_bytes);

	const bool started = rt.dispatch(p, n);
	if (started && !rt.is_loop_running()) {
		// We're (re-)entering the loop. This call only returns when
		// the user closes the window. Inside the loop, OCaml
		// callbacks may themselves call back into
		// declgl_ship_backend_cmd — that's fine, dispatch is
		// reentrant since it doesn't touch the loop_running flag
		// when no StartRegl is present.
		rt.run();
	}

	CAMLreturn(Val_unit);
}

extern "C" CAMLprim value declgl_ship_audio_cmd(value v_bytes)
{
	CAMLparam1(v_bytes);

	const uint8_t *p =
		reinterpret_cast<const uint8_t *>(Bytes_val(v_bytes));
	const size_t n = caml_string_length(v_bytes);

	runtime().dispatch_audio(p, n);

	CAMLreturn(Val_unit);
}
