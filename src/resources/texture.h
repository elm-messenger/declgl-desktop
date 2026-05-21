#pragma once

// resources/texture.h — RAII GL texture handle.
//
// Mirrors the JS backend's per-name texture object (one image, one
// mip chain, optional crop region). Ownership is exclusive: deleting
// a Texture destroys the underlying GL object.

#include <glad/gl.h>

#include <cstdint>
#include <string>

namespace declgl
{

// Filter modes mirror mlregl::transport::backend::TextureOptions.
// Unknown / unsupported values fall through to LINEAR.
enum class TextureFilter {
	Nearest,
	Linear,
	NearestMipmapNearest,
	NearestMipmapLinear,
	LinearMipmapNearest,
	LinearMipmapLinear,
};

class Texture {
    public:
	Texture() = default;
	Texture(const Texture &) = delete;
	Texture &operator=(const Texture &) = delete;
	Texture(Texture &&other) noexcept;
	Texture &operator=(Texture &&other) noexcept;
	~Texture();

	// Upload an RGBA8 buffer to a freshly-created GL_TEXTURE_2D.
	// [pixels] must point to width*height*4 bytes (no row stride
	// padding). Returns false on any GL error; on success [id_] is a
	// valid texture name.
	//
	// [generate_mipmaps] is true iff [min_filter] is one of the four
	// mipmap modes — the caller decides; we don't infer.
	//
	// [premultiply_alpha] (default true): when set, the loader makes
	// a transient CPU-side copy of the buffer with each RGB channel
	// multiplied by alpha (rounded), so the texture is stored in
	// premultiplied form. This is the correct representation for the
	// engine's (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) blend equation and it
	// also avoids the straight-alpha edge-bleed that bilinear
	// filtering otherwise produces around translucent edges. Pass
	// false for non-colour data (e.g. SDF/MSDF atlases) where
	// multiplying RGB by alpha would corrupt the signal.
	bool upload_rgba8(int width, int height, const uint8_t *pixels,
			  TextureFilter min_filter, TextureFilter mag_filter,
			  bool generate_mipmaps, bool premultiply_alpha = true);

	GLuint id() const
	{
		return id_;
	}
	int width() const
	{
		return width_;
	}
	int height() const
	{
		return height_;
	}
	bool ok() const
	{
		return id_ != 0;
	}

    private:
	GLuint id_ = 0;
	int width_ = 0;
	int height_ = 0;
};

} // namespace declgl
