#pragma once

// resources/image_decoder.h — thin wrapper around stb_image.
//
// Converts a path on disk into RGBA8 pixels, optionally clipped to a
// sub-rectangle (matching mlregl::transport::backend::TextureCrop).
// Memory ownership is RAII: the buffer frees itself on destruction.

#include <cstdint>
#include <memory>
#include <string>

namespace declgl
{

// Optional sub-region to extract from the source image. All four
// fields in pixels of the *source* image. If [width] or [height] is
// zero or negative the crop is treated as absent.
struct ImageCrop {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// Owned RGBA8 pixel buffer + dimensions.
struct DecodedImage {
	std::unique_ptr<uint8_t[], void (*)(uint8_t *)> pixels{
		nullptr, +[](uint8_t *) {}
	};
	int width = 0;
	int height = 0;

	bool ok() const
	{
		return pixels != nullptr;
	}
};

// Read [path] from disk, decode (PNG/JPG/BMP/TGA/PSD/GIF), force RGBA8
// output. If [crop] specifies a region inside the source the pixels
// are copied to a fresh contiguous RGBA8 buffer of that size; on
// out-of-bounds the crop is silently clipped to the image rectangle.
//
// On any failure (file missing, decode error, OOM) returns an empty
// DecodedImage; check [.ok()]. Diagnostic output goes to stderr.
DecodedImage decode_image_file(const std::string &path, const ImageCrop &crop);

} // namespace declgl
