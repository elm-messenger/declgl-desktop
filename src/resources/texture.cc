#include "resources/texture.h"

#include <glad/gl.h>

#include <cstdio>
#include <utility>
#include <vector>

#include "log/log.h"

namespace declgl
{

namespace
{

GLint to_gl_filter(TextureFilter f)
{
	switch (f) {
	case TextureFilter::Nearest:
		return GL_NEAREST;
	case TextureFilter::Linear:
		return GL_LINEAR;
	case TextureFilter::NearestMipmapNearest:
		return GL_NEAREST_MIPMAP_NEAREST;
	case TextureFilter::NearestMipmapLinear:
		return GL_NEAREST_MIPMAP_LINEAR;
	case TextureFilter::LinearMipmapNearest:
		return GL_LINEAR_MIPMAP_NEAREST;
	case TextureFilter::LinearMipmapLinear:
		return GL_LINEAR_MIPMAP_LINEAR;
	}
	return GL_LINEAR;
}

// Multiply each texel's RGB by its alpha, rounded so 0xff*0xff/0xff = 0xff
// exactly. This is the standard "+ 127 / 255" rounding from libpng / pillow
// — the exact same constant the JS canvas / WebGL `premultiplyAlpha: true`
// path uses internally, so behaviour stays bit-equivalent across backends.
//
// We allocate a fresh std::vector instead of mutating the caller's buffer
// because the caller's buffer can be a stb_image-owned arena that the
// renderer expects to remain straight-alpha (e.g. for re-decoding into
// other crops or for downstream debugging dumps). Allocation cost is
// O(width*height*4) bytes once at upload — negligible.
std::vector<uint8_t> premultiply_rgba(const uint8_t *src, int n_pixels)
{
	std::vector<uint8_t> out(static_cast<size_t>(n_pixels) * 4);
	for (size_t i = 0; i < out.size(); i += 4) {
		const uint8_t a = src[i + 3];
		// (x*a + 127) / 255: rounded division, exact at the 0 and 255
		// endpoints, and bit-equivalent to the formula libpng uses.
		out[i + 0] = static_cast<uint8_t>((src[i + 0] * a + 127) / 255);
		out[i + 1] = static_cast<uint8_t>((src[i + 1] * a + 127) / 255);
		out[i + 2] = static_cast<uint8_t>((src[i + 2] * a + 127) / 255);
		out[i + 3] = a;
	}
	return out;
}

} // namespace

Texture::Texture(Texture &&other) noexcept
	: id_(std::exchange(other.id_, 0)),
	  width_(std::exchange(other.width_, 0)),
	  height_(std::exchange(other.height_, 0))
{
}

Texture &Texture::operator=(Texture &&other) noexcept
{
	if (this != &other) {
		if (id_)
			glDeleteTextures(1, &id_);
		id_ = std::exchange(other.id_, 0);
		width_ = std::exchange(other.width_, 0);
		height_ = std::exchange(other.height_, 0);
	}
	return *this;
}

Texture::~Texture()
{
	if (id_)
		glDeleteTextures(1, &id_);
}

bool Texture::upload_rgba8(int width, int height, const uint8_t *pixels,
			   TextureFilter min_filter, TextureFilter mag_filter,
			   bool generate_mipmaps, bool premultiply_alpha)
{
	if (id_) {
		glDeleteTextures(1, &id_);
		id_ = 0;
	}
	if (!pixels || width <= 0 || height <= 0)
		return false;

	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (!tex)
		return false;

	glBindTexture(GL_TEXTURE_2D, tex);

	// Pack alignment 1 because we don't pad scanlines.
	GLint prev_unpack = 4;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// Upload either the caller's buffer or a transient premultiplied
	// copy. The vector is held in scope until after [glTexImage2D]
	// returns so its storage stays valid; the driver only needs the
	// pointer for the duration of that call.
	const uint8_t *upload_ptr = pixels;
	std::vector<uint8_t> pm_buffer;
	if (premultiply_alpha) {
		pm_buffer = premultiply_rgba(pixels, width * height);
		upload_ptr = pm_buffer.data();
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, upload_ptr);

	glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			to_gl_filter(min_filter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			to_gl_filter(mag_filter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (generate_mipmaps) {
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	glBindTexture(GL_TEXTURE_2D, 0);

	if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
		DECLGL_LOG_ERROR("glTexImage2D error 0x{:x} for {}x{}", err,
				 width, height);
		glDeleteTextures(1, &tex);
		return false;
	}

	id_ = tex;
	width_ = width;
	height_ = height;
	return true;
}

} // namespace declgl
