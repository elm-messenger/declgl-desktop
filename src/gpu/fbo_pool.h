#pragma once

// gpu/fbo_pool.h — pool of fixed-count, RGBA8 + framebuffer "palettes".
//
// Mirrors ml-regl-js/src/app.js's [fbos] / [freePalette] arrays: a
// flat collection of N FBO+texture pairs, all sized to the system
// framebuffer's drawing buffer. Each frame the renderer marks every
// palette free, then [acquire] / [release]s them through the
// [drawRenderable] / [drawGroup] / [drawComp] recursion.
//
// The pool is *exact* — JS allocates exactly `fbo_num` palettes and
// throws if the renderer asks for more (`getFreePalette` returns -1).
// We mirror that invariant; the engine logs a warning and the
// affected sub-tree silently drops.

#include <glad/gl.h>

#include <cstddef>
#include <vector>

namespace declgl
{

// One palette = one GL framebuffer with a single RGBA8 colour
// attachment, no depth/stencil. RAII; deleting frees both.
struct Fbo {
	GLuint framebuffer = 0;
	GLuint texture = 0;
	int width = 0;
	int height = 0;
};

class FboPool {
    public:
	FboPool() = default;
	~FboPool();

	FboPool(const FboPool &) = delete;
	FboPool &operator=(const FboPool &) = delete;

	// Allocate [count] palettes at the given pixel size. Replaces any
	// previously-allocated set. All palettes start free.
	bool init(int count, int width, int height);

	// Resize every palette to a new pixel size (cheap if unchanged).
	// Called once per frame from the engine main loop.
	void resize_all(int width, int height);

	// Free every palette in O(N). Done at the start of every frame.
	void free_all();

	// Acquire a free palette, returns its id. -1 on exhaustion.
	int acquire();

	// Mark the given palette free again. Out-of-range ids are silently
	// ignored.
	void release(int id);

	// Direct accessor. id < 0 (or out-of-range) returns nullptr.
	const Fbo *get(int id) const;

	int size() const
	{
		return static_cast<int>(fbos_.size());
	}

    private:
	std::vector<Fbo> fbos_;
	std::vector<bool> free_;

	void destroy_all();
	static bool create_fbo(Fbo &out, int width, int height);
};

} // namespace declgl
