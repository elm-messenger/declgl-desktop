#pragma once

#include <quickjs.h>

#include <string>

namespace mlregl::transport::backend
{
class BackendCommand;
class BackendEvent;
class Event;
}
namespace mlregl::transport::render
{
class Renderable;
}

namespace declgl::elm
{

bool command_from_js(JSContext *ctx, JSValueConst value,
		     const std::string &app_name, bool fullscreen,
		     mlregl::transport::backend::BackendCommand &out,
		     std::string &error);
bool view_from_js(JSContext *ctx, JSValueConst value,
		  mlregl::transport::render::Renderable &out,
		  std::string &error);
JSValue
backend_event_to_js(JSContext *ctx,
		    const mlregl::transport::backend::BackendEvent &event,
		    std::string &error);
JSValue input_event_to_js(JSContext *ctx,
			  const mlregl::transport::backend::Event &event,
			  std::string &type, std::string &error);

} // namespace declgl::elm
