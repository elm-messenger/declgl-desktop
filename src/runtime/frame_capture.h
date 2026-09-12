#pragma once

#include <cstdint>
#include <filesystem>

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

} // namespace declgl
