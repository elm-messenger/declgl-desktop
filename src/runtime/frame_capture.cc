#include "runtime/frame_capture.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <nlohmann/json.hpp>

#include "log/log.h"
#include "transport_render.pb.h"

namespace declgl
{

namespace
{

using json = nlohmann::json;
using mlregl::transport::common::Value;
using mlregl::transport::render::AtomicRenderable;
using mlregl::transport::render::Effect;
using mlregl::transport::render::ProgramCallField;
using mlregl::transport::render::Renderable;

json value_to_json(const Value &value)
{
	switch (value.kind_case()) {
	case Value::kNumberValue:
		return { { "number", value.number_value() } };
	case Value::kStringValue:
		return { { "string", value.string_value() } };
	case Value::kNumberArrayValue: {
		json values = json::array();
		for (double item : value.number_array_value().values())
			values.push_back(item);
		return { { "numbers", std::move(values) } };
	}
	case Value::kBoolValue:
		return { { "bool", value.bool_value() } };
	case Value::kStringArrayValue: {
		json values = json::array();
		for (const auto &item : value.string_array_value().values())
			values.push_back(item);
		return { { "strings", std::move(values) } };
	}
	case Value::KIND_NOT_SET:
	default:
		return nullptr;
	}
}

json field_to_json(const ProgramCallField &field)
{
	json result{ { "key", field.key() } };
	result["value"] = field.has_val() ? value_to_json(field.val()) : json{};
	return result;
}

template <typename Program>
json program_to_json(const Program &program)
{
	json fields = json::array();
	for (const auto &field : program.fields())
		fields.push_back(field_to_json(field));
	return { { "program", program.program() },
		 { "fields", std::move(fields) } };
}

json renderable_to_json(const Renderable &tree)
{
	switch (tree.kind_case()) {
	case Renderable::kAtomic:
		return { { "atomic", program_to_json(tree.atomic()) } };
	case Renderable::kGroup: {
		const auto &group = tree.group();
		json effects = json::array();
		for (const Effect &effect : group.effects())
			effects.push_back(program_to_json(effect));

		json children = json::array();
		for (const Renderable &child : group.children())
			children.push_back(renderable_to_json(child));

		json result{ { "effects", std::move(effects) },
			     { "children", std::move(children) } };
		if (group.has_camera()) {
			const auto &camera = group.camera();
			result["camera"] = { { "x", camera.x() },
					     { "y", camera.y() },
					     { "zoom", camera.zoom() },
					     { "rotation", camera.rotation() } };
		}
		return { { "group", std::move(result) } };
	}
	case Renderable::kComposite: {
		const auto &composite = tree.composite();
		json result = json::object();
		if (composite.has_compositor())
			result["compositor"] =
				program_to_json(composite.compositor());
		if (composite.has_left())
			result["left"] = renderable_to_json(composite.left());
		if (composite.has_right())
			result["right"] = renderable_to_json(composite.right());
		return { { "composite", std::move(result) } };
	}
	case Renderable::KIND_NOT_SET:
	default:
		return { { "empty", true } };
	}
}

std::string frame_stem(std::uint64_t frame_number)
{
	std::ostringstream name;
	name << "frame_" << std::setfill('0') << std::setw(8) << frame_number;
	return name.str();
}

bool save_render_tree(const std::filesystem::path &path, const Renderable &tree)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
		return false;
	output << renderable_to_json(tree).dump(2) << '\n';
	return static_cast<bool>(output);
}

bool save_back_buffer(const std::filesystem::path &path, SDL_Window *window)
{
	int width = 0;
	int height = 0;
	if (!window || !SDL_GetWindowSizeInPixels(window, &width, &height) ||
	    width <= 0 || height <= 0) {
		return false;
	}

	const auto pixel_width = static_cast<std::size_t>(width);
	const auto pixel_height = static_cast<std::size_t>(height);
	if (pixel_width >
	    std::numeric_limits<std::size_t>::max() / 4 / pixel_height) {
		return false;
	}
	std::vector<std::uint8_t> pixels(pixel_width * pixel_height * 4);

	GLint previous_read_fbo = 0;
	GLint previous_pack_alignment = 4;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		     pixels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER,
			  static_cast<GLuint>(previous_read_fbo));

	SDL_Surface *surface = SDL_CreateSurfaceFrom(
		width, height, SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4);
	if (!surface)
		return false;
	const bool saved = SDL_FlipSurface(surface, SDL_FLIP_VERTICAL) &&
			   SDL_SaveBMP(surface, path.string().c_str());
	SDL_DestroySurface(surface);
	return saved;
}

} // namespace

void FrameCapture::configure_from_environment()
{
	interval_ = 0;
	format_ = Format::Both;
	output_dir_.clear();

	const char *raw = std::getenv("DECLGL_SAVE_FRAME");
	if (!raw || !*raw)
		return;

	const std::string_view value(raw);
	std::uint64_t interval = 0;
	const auto parsed =
		std::from_chars(value.data(), value.data() + value.size(), interval);
	if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
		DECLGL_LOG_WARN(
			"DECLGL_SAVE_FRAME must be a non-negative integer; got '{}'",
			value);
		return;
	}
	if (interval == 0)
		return;

	const char *raw_format = std::getenv("DECLGL_SAVE_FRAME_FORMAT");
	if (raw_format && *raw_format) {
		const std::string_view format(raw_format);
		if (format == "bmp") {
			format_ = Format::Bmp;
		} else if (format == "rendertree") {
			format_ = Format::RenderTree;
		} else if (format != "both") {
			DECLGL_LOG_WARN(
				"DECLGL_SAVE_FRAME_FORMAT must be 'bmp', "
				"'rendertree', or 'both'; got '{}'",
				format);
			return;
		}
	}

	std::error_code ec;
	output_dir_ = std::filesystem::current_path(ec) / "frames";
	if (ec) {
		DECLGL_LOG_ERROR("save_frame: cannot read working directory: {}",
				 ec.message());
		output_dir_.clear();
		return;
	}
	std::filesystem::create_directories(output_dir_, ec);
	if (ec) {
		DECLGL_LOG_ERROR("save_frame: cannot create '{}': {}",
				 output_dir_.string(), ec.message());
		output_dir_.clear();
		return;
	}

	interval_ = interval;
	const char *format_name = "both";
	if (format_ == Format::Bmp)
		format_name = "bmp";
	else if (format_ == Format::RenderTree)
		format_name = "rendertree";
	DECLGL_LOG_INFO("saving every {} frame(s) as {} to '{}'", interval_,
			format_name, output_dir_.string());
}

void FrameCapture::capture(std::uint64_t frame_number, SDL_Window *window,
			   const Renderable &tree) const
{
	if (interval_ == 0 || frame_number % interval_ != 0)
		return;

	const std::string stem = frame_stem(frame_number);
	const auto tree_path = output_dir_ / (stem + ".render-tree.json");
	const auto image_path = output_dir_ / (stem + ".bmp");

	if (format_ != Format::Bmp && !save_render_tree(tree_path, tree)) {
		DECLGL_LOG_ERROR("save_frame: failed to write '{}'",
				 tree_path.string());
	}
	if (format_ != Format::RenderTree &&
	    !save_back_buffer(image_path, window)) {
		DECLGL_LOG_ERROR("save_frame: failed to write '{}': {}",
				 image_path.string(), SDL_GetError());
	}
}

} // namespace declgl
