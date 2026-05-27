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
	cur_w_ = 0;
	cur_h_ = 0;
}

bool FboPool::init(int count, int width, int height)
{
	destroy_all();
	cur_w_ = width;
	cur_h_ = height;
	// Reserve up to the hard cap so later [acquire] growth via
	// `push_back` never reallocates fbos_ / free_, keeping any
	// `const Fbo *` returned by [get] stable across growth.
	fbos_.reserve(static_cast<size_t>(kMaxFbos));
	free_.reserve(static_cast<size_t>(kMaxFbos));
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
	cur_w_ = width;
	cur_h_ = height;
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

	// At the hard cap: this is a runtime error; the affected sub-tree
	// silently drops in the walker. Log every miss so the operator
	// notices an over-spending frame instead of it being papered over.
	if (static_cast<int>(fbos_.size()) >= kMaxFbos) {
		DECLGL_LOG_ERROR(
			"FBO pool exhausted at hard cap {} (size={}); "
			"the affected sub-tree will drop. Reduce effect/"
			"composite depth or raise StartRegl.fbo_num.",
			kMaxFbos, static_cast<int>(fbos_.size()));
		return -1;
	}

	// No free slot but below the cap: warn, then grow by one. We size
	// the new palette to the current pool dimensions so it can be
	// used immediately by the in-flight render walk.
	const int new_size = static_cast<int>(fbos_.size()) + 1;
	DECLGL_LOG_WARN("FBO pool full (size={}); growing to {} (cap={}). "
			"Consider raising StartRegl.fbo_num to avoid mid-frame "
			"GL allocations.",
			static_cast<int>(fbos_.size()), new_size, kMaxFbos);

	Fbo nf;
	if (!create_fbo(nf, cur_w_, cur_h_)) {
		DECLGL_LOG_ERROR(
			"FBO pool growth failed: create_fbo({}x{}) returned "
			"false; the affected sub-tree will drop.",
			cur_w_, cur_h_);
		return -1;
	}
	fbos_.push_back(nf);
	free_.push_back(false);
	return static_cast<int>(fbos_.size()) - 1;
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
