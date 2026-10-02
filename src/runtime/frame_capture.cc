#include "runtime/frame_capture.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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
#include "renderer/render_context.h"
#include "stb_image_write.h"
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

// Read the window's back buffer as RGBA, top row first.
bool read_back_buffer(SDL_Window *window, int &width, int &height,
		      std::vector<std::uint8_t> &out)
{
	if (!window || !SDL_GetWindowSizeInPixels(window, &width, &height) ||
	    width <= 0 || height <= 0)
		return false;
	const auto w = static_cast<std::size_t>(width);
	const auto h = static_cast<std::size_t>(height);
	if (w > std::numeric_limits<std::size_t>::max() / 4 / h)
		return false;
	std::vector<std::uint8_t> bottom_up(w * h * 4);

	GLint previous_read_fbo = 0;
	GLint previous_pack_alignment = 4;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		     bottom_up.data());
	glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER,
			  static_cast<GLuint>(previous_read_fbo));

	out.resize(bottom_up.size());
	for (std::size_t y = 0; y < h; ++y)
		std::copy_n(bottom_up.data() + (h - 1 - y) * w * 4, w * 4,
			    out.data() + y * w * 4);
	return true;
}

// Box-filter resize of an RGBA region: each output pixel averages the
// source pixels it covers, which keeps small text readable when shrinking.
std::vector<std::uint8_t> resize_box(const std::uint8_t *src, int stride,
				     int sw, int sh, int dw, int dh)
{
	std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4);
	const double fx = static_cast<double>(sw) / dw;
	const double fy = static_cast<double>(sh) / dh;
	for (int y = 0; y < dh; ++y) {
		const double y0 = y * fy;
		const double y1 = std::min<double>(sh, (y + 1) * fy);
		for (int x = 0; x < dw; ++x) {
			const double x0 = x * fx;
			const double x1 = std::min<double>(sw, (x + 1) * fx);
			double acc[4] = { 0, 0, 0, 0 };
			double total = 0.0;
			for (int py = static_cast<int>(y0);
			     py < static_cast<int>(std::ceil(y1)); ++py) {
				const double wy = std::min<double>(y1, py + 1) -
						  std::max<double>(y0, py);
				for (int px = static_cast<int>(x0);
				     px < static_cast<int>(std::ceil(x1)); ++px) {
					const double wt =
						wy * (std::min<double>(x1, px + 1) -
						      std::max<double>(x0, px));
					const std::uint8_t *p =
						src + static_cast<std::size_t>(py) * stride +
						static_cast<std::size_t>(px) * 4;
					for (int c = 0; c < 4; ++c)
						acc[c] += p[c] * wt;
					total += wt;
				}
			}
			std::uint8_t *o =
				out.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
			for (int c = 0; c < 4; ++c)
				o[c] = static_cast<std::uint8_t>(
					std::lround(acc[c] / total));
		}
	}
	return out;
}

bool save_back_buffer(const std::filesystem::path &path, SDL_Window *window)
{
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> pixels;
	if (!read_back_buffer(window, width, height, pixels))
		return false;
	SDL_Surface *surface = SDL_CreateSurfaceFrom(
		width, height, SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4);
	if (!surface)
		return false;
	const bool saved = SDL_SaveBMP(surface, path.string().c_str());
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

std::string renderable_to_json_string(const Renderable &tree)
{
	return renderable_to_json(tree).dump();
}

bool save_screenshot(SDL_Window *window, const std::filesystem::path &path)
{
	return save_back_buffer(path, window);
}

ScreenshotResult capture_screenshot(SDL_Window *window, double virt_w,
				    double virt_h,
				    const ScreenshotOptions &options,
				    const std::filesystem::path &path)
{
	ScreenshotResult result;
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> pixels;
	if (!read_back_buffer(window, width, height, pixels)) {
		result.error = "screenshot failed";
		return result;
	}
	for (std::size_t i = 3; i < pixels.size(); i += 4)
		pixels[i] = 255;

	compute_fit_rect(width, height, virt_w, virt_h, result.view_x,
			 result.view_y, result.view_w, result.view_h);
	// Window pixels per virtual unit.
	const double ppu = virt_w > 0.0 ? result.view_w / virt_w : 1.0;
	double cx = 0.0, cy = 0.0, cw = width, ch = height;
	if (options.view || options.has_region) {
		cx = result.view_x;
		cy = result.view_y;
		cw = result.view_w;
		ch = result.view_h;
	}
	if (options.has_region) {
		cx += options.region_x * ppu;
		cy += options.region_y * ppu;
		cw = options.region_w * ppu;
		ch = options.region_h * ppu;
	}
	const int x0 = std::clamp(static_cast<int>(std::lround(cx)), 0, width);
	const int y0 = std::clamp(static_cast<int>(std::lround(cy)), 0, height);
	const int x1 = std::clamp(static_cast<int>(std::lround(cx + cw)), 0, width);
	const int y1 = std::clamp(static_cast<int>(std::lround(cy + ch)), 0, height);
	if (x1 <= x0 || y1 <= y0) {
		result.error = "the region is outside the view";
		return result;
	}
	const int sw = x1 - x0;
	const int sh = y1 - y0;

	// Output size: the captured pixels, or the requested size in virtual
	// units; never up, and at most max_width wide.
	double tw = sw;
	double th = sh;
	if (options.virtual_scale && ppu > 0.0) {
		tw = options.has_region ? options.region_w :
		     options.view       ? virt_w :
					  sw / ppu;
		th = options.has_region ? options.region_h :
		     options.view       ? virt_h :
					  sh / ppu;
	}
	if (tw > sw) {
		th *= sw / tw;
		tw = sw;
	}
	if (options.max_width > 0 && tw > options.max_width) {
		th *= options.max_width / tw;
		tw = options.max_width;
	}
	const int dw = std::max(1, static_cast<int>(std::lround(tw)));
	const int dh = std::max(1, static_cast<int>(std::lround(th)));
	const std::uint8_t *origin =
		pixels.data() +
		(static_cast<std::size_t>(y0) * width + x0) * 4;
	std::vector<std::uint8_t> image =
		resize_box(origin, width * 4, sw, sh, dw, dh);

	bool saved = false;
	const std::string file = path.string();
	if (options.format == ScreenshotOptions::Format::Jpeg) {
		saved = stbi_write_jpg(file.c_str(), dw, dh, 4, image.data(),
				       std::clamp(options.quality, 1, 100)) != 0;
	} else {
		SDL_Surface *surface = SDL_CreateSurfaceFrom(
			dw, dh, SDL_PIXELFORMAT_RGBA32, image.data(), dw * 4);
		if (surface) {
			saved = options.format == ScreenshotOptions::Format::Png ?
					SDL_SavePNG(surface, file.c_str()) :
					SDL_SaveBMP(surface, file.c_str());
			SDL_DestroySurface(surface);
		}
	}
	if (!saved) {
		result.error = "screenshot failed";
		return result;
	}
	result.ok = true;
	result.width = dw;
	result.height = dh;
	result.pixels_per_unit = ppu * dw / sw;
	return result;
}

} // namespace declgl
