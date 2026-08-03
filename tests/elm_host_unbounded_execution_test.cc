#include <iostream>

#include "elm_host/elm_host.h"
#include "transport_backend.pb.h"

namespace
{

template<typename Event, typename Callback>
void deliver(const Event &event, Callback &&callback)
{
	std::string bytes;
	event.SerializeToString(&bytes);
	callback(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

} // namespace

int main()
{
	declgl::ElmHostConfig config;
	config.script = UNBOUNDED_EXECUTION_SCRIPT;
	config.module = "Main";
	config.app_name = "elm-host-unbounded-execution-test";

	declgl::ElmHost host(std::move(config));
	if (!host.initialize()) {
		std::cerr << "initialize failed: " << host.error() << '\n';
		return 1;
	}

	mlregl::transport::backend::Event mouse_event;
	auto *mouse = mouse_event.mutable_mouse_up();
	mouse->set_button(1);
	mouse->set_x(10);
	mouse->set_y(10);
	deliver(mouse_event, [&](const uint8_t *bytes, std::size_t size) {
		host.deliver_event(bytes, size);
	});

	mlregl::transport::backend::Event tick_event;
	tick_event.mutable_update_tick()->set_ts(1000);
	deliver(tick_event, [&](const uint8_t *bytes, std::size_t size) {
		host.deliver_event(bytes, size);
	});

	mlregl::transport::backend::BackendEvent backend_event;
	backend_event.mutable_program_created()->set_name("test-program");
	deliver(backend_event, [&](const uint8_t *bytes, std::size_t size) {
		host.on_backend_event(bytes, size);
	});

	if (!host.ok()) {
		std::cerr << "unbounded execution failed: " << host.error() << '\n';
		return 1;
	}
	return 0;
}
