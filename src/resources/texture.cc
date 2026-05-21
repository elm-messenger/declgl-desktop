#include "resources/texture.h"

#include <glad/gl.h>

#include <cstdio>
#include <utility>

namespace declgl {

namespace {

GLint to_gl_filter(TextureFilter f) {
    switch (f) {
        case TextureFilter::Nearest:               return GL_NEAREST;
        case TextureFilter::Linear:                return GL_LINEAR;
        case TextureFilter::NearestMipmapNearest:  return GL_NEAREST_MIPMAP_NEAREST;
        case TextureFilter::NearestMipmapLinear:   return GL_NEAREST_MIPMAP_LINEAR;
        case TextureFilter::LinearMipmapNearest:   return GL_LINEAR_MIPMAP_NEAREST;
        case TextureFilter::LinearMipmapLinear:    return GL_LINEAR_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

}  // namespace

Texture::Texture(Texture&& other) noexcept
    : id_(std::exchange(other.id_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (id_) glDeleteTextures(1, &id_);
        id_     = std::exchange(other.id_, 0);
        width_  = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

Texture::~Texture() {
    if (id_) glDeleteTextures(1, &id_);
}

bool Texture::upload_rgba8(int width, int height,
                           const uint8_t* pixels,
                           TextureFilter min_filter,
                           TextureFilter mag_filter,
                           bool generate_mipmaps) {
    if (id_) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
    if (!pixels || width <= 0 || height <= 0) return false;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return false;

    glBindTexture(GL_TEXTURE_2D, tex);

    // Pack alignment 1 because we don't pad scanlines.
    GLint prev_unpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, to_gl_filter(min_filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, to_gl_filter(mag_filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    if (generate_mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        std::fprintf(stderr, "[declgl/texture] glTexImage2D error 0x%x for %dx%d\n",
                     err, width, height);
        glDeleteTextures(1, &tex);
        return false;
    }

    id_     = tex;
    width_  = width;
    height_ = height;
    return true;
}

}  // namespace declgl
