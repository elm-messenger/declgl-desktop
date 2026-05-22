// gpu/fbo_pool.cc

#include "gpu/fbo_pool.h"

#include <cstdio>

#include "log/log.h"

namespace declgl
{

FboPool::~FboPool()
{
	destroy_all();
}

bool FboPool::create_fbo(Fbo &out, int width, int height)
{
	glGenTextures(1, &out.texture);
	glBindTexture(GL_TEXTURE_2D, out.texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, nullptr);
	// The compositor & effect shaders use linear filtering for the
	// fade/blur paths to look smooth across resolution changes; the
	// palette passthrough doesn't care, so picking LINEAR uniformly
	// matches the JS regl default and avoids per-effect special cases.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &out.framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, out.framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, out.texture, 0);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	out.width = width;
	out.height = height;

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		DECLGL_LOG_ERROR("incomplete framebuffer (status=0x{:x})",
				 status);
		return false;
	}
	return true;
}

void FboPool::destroy_all()
{
	for (auto &f : fbos_) {
		if (f.framebuffer)
			glDeleteFramebuffers(1, &f.framebuffer);
		if (f.texture)
			glDeleteTextures(1, &f.texture);
		f = Fbo{};
	}
	fbos_.clear();
	free_.clear();
}

bool FboPool::init(int count, int width, int height)
{
	destroy_all();
	if (count <= 0)
		return true;
	fbos_.resize(static_cast<size_t>(count));
	free_.assign(static_cast<size_t>(count), true);
	for (auto &f : fbos_) {
		if (!create_fbo(f, width, height)) {
			destroy_all();
			return false;
		}
	}
	return true;
}

void FboPool::resize_all(int width, int height)
{
	for (auto &f : fbos_) {
		if (f.width == width && f.height == height)
			continue;
		glBindTexture(GL_TEXTURE_2D, f.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
			     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindTexture(GL_TEXTURE_2D, 0);
		f.width = width;
		f.height = height;
	}
}

void FboPool::free_all()
{
	std::fill(free_.begin(), free_.end(), true);
}

int FboPool::acquire()
{
	for (size_t i = 0; i < free_.size(); ++i) {
		if (free_[i]) {
			free_[i] = false;
			return static_cast<int>(i);
		}
	}
	static bool warned = false;
	if (!warned) {
		DECLGL_LOG_WARN(
			"pool exhausted (size={}). Increase "
			"StartRegl.fbo_num. This warning fires once per "
			"process; subsequent failures are silent and the "
			"affected subtree drops.",
			static_cast<int>(fbos_.size()));
		warned = true;
	}
	return -1;
}

void FboPool::release(int id)
{
	if (id < 0 || id >= static_cast<int>(free_.size()))
		return;
	free_[static_cast<size_t>(id)] = true;
}

const Fbo *FboPool::get(int id) const
{
	if (id < 0 || id >= static_cast<int>(fbos_.size()))
		return nullptr;
	return &fbos_[static_cast<size_t>(id)];
}

} // namespace declgl
