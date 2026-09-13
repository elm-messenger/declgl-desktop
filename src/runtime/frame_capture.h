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

} // namespace declgl
