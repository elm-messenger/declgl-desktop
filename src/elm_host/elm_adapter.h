#pragma once

#include <quickjs.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mlregl::transport::audio
{
class AudioBackendEvent;
class AudioCommandBatch;
}

namespace mlregl::transport::backend
{
class BackendCommand;
class BackendCommandBatch;
class BackendEvent;
class Event;
}
namespace mlregl::transport::render
{
class Renderable;
}

namespace declgl::elm
{

struct AudioLoadRequest {
	std::string url;
	int64_t request_id;
};

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
bool audio_batch_from_js(
	JSContext *ctx, JSValueConst value,
	mlregl::transport::audio::AudioCommandBatch &audio,
	mlregl::transport::backend::BackendCommandBatch &loads,
	std::vector<AudioLoadRequest> &requests, std::string &error);
JSValue audio_event_to_js(
	JSContext *ctx,
	const mlregl::transport::audio::AudioBackendEvent &event,
	std::optional<int64_t> request_id, std::string &error);

} // namespace declgl::elm
