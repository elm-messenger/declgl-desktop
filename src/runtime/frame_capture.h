#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct SDL_Window;

namespace mlregl::transport::render
{
class Renderable;
}

namespace declgl
{

class FrameCapture {
    public:
	void configure_from_environment();
	void capture(std::uint64_t frame_number, SDL_Window *window,
		     const mlregl::transport::render::Renderable &tree) const;

    private:
	enum class Format {
		Bmp,
		RenderTree,
		Both,
	};

	std::uint64_t interval_ = 0;
	Format format_ = Format::Both;
	std::filesystem::path output_dir_;
};

// JSON representation used by frame capture and the optional control
// protocol. This is deliberately separate from the protobuf wire format so
// tools can inspect render trees without decoding protobufs.
std::string renderable_to_json_string(
	const mlregl::transport::render::Renderable &tree);

// Capture the currently-bound desktop back buffer as a BMP. Must be called on
// the GL/render thread while the window's context is current.
bool save_screenshot(SDL_Window *window, const std::filesystem::path &path);

// What a control-protocol screenshot captures and how it is written.
struct ScreenshotOptions {
	enum class Format { Bmp, Png, Jpeg };
	// Crop to the letterboxed virtual area instead of the whole window.
	bool view = false;
	// Part of the view in virtual units (implies [view]).
	bool has_region = false;
	double region_x = 0.0, region_y = 0.0, region_w = 0.0, region_h = 0.0;
	// Scale to one pixel per virtual unit (never up).
	bool virtual_scale = false;
	// Cap on the output width in pixels; 0 = none.
	int max_width = 0;
	Format format = Format::Bmp;
	int quality = 90; // JPEG, 1..100
};

struct ScreenshotResult {
	bool ok = false;
	std::string error;
	int width = 0, height = 0; // the written image
	// The virtual area in window pixels.
	int view_x = 0, view_y = 0, view_w = 0, view_h = 0;
	// Written pixels per virtual unit.
	double pixels_per_unit = 0.0;
};

// Capture the back buffer as [options] say, opaque (as the window shows it),
// into [path]. Same threading rule as [save_screenshot].
ScreenshotResult capture_screenshot(SDL_Window *window, double virt_w,
				    double virt_h,
				    const ScreenshotOptions &options,
				    const std::filesystem::path &path);

} // namespace declgl
