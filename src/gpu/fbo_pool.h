#pragma once

// gpu/fbo_pool.h — pool of RGBA8 + framebuffer "palettes" that seeds
// at `fbo_num` and grows on demand up to a hard cap.
//
// Mirrors ml-regl-js/src/app.js's [fbos] / [freePalette] arrays: a
// flat collection of N FBO+texture pairs, all sized to the system
// framebuffer's drawing buffer. Each frame the renderer marks every
// palette free, then [acquire] / [release]s them through the
// [drawRenderable] / [drawGroup] / [drawComp] recursion.
//
// Growth: when [acquire] finds no free slot, it logs a warning and
// allocates a new palette at the current pool dimensions, up to
// [kMaxFbos] (matches the JS backend's 1000-palette cap in
// `getFreePalette`). At the cap, [acquire] logs an error and returns
// -1 (the affected sub-tree silently drops).
//
// Pointer stability: [init] reserves capacity for [kMaxFbos] up front,
// so `push_back`s during growth never reallocate the underlying
// storage. This keeps any `const Fbo *` returned by [get] valid even
// if the pool grows later in the same frame. The reserve only costs
// a small CPU-side bookkeeping vector; the expensive GPU memory
// (RGBA8 textures) is still allocated lazily inside [create_fbo],
// only as new palettes are actually created.

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
	// Hard cap on pool size. Matches the 1000-palette ceiling in
	// ml-regl-js/src/app.js `getFreePalette`. Reaching this is
	// treated as an unrecoverable runtime error for the affected
	// frame's sub-tree.
	static constexpr int kMaxFbos = 1000;

	FboPool() = default;
	~FboPool();

	FboPool(const FboPool &) = delete;
	FboPool &operator=(const FboPool &) = delete;

	// Allocate [count] palettes at the given pixel size. Replaces any
	// previously-allocated set. All palettes start free. Reserves
	// capacity for [kMaxFbos] so subsequent growth in [acquire] does
	// not invalidate `const Fbo *` references handed out by [get].
	bool init(int count, int width, int height);

	// Resize every palette to a new pixel size (cheap if unchanged).
	// Called once per frame from the engine main loop. Also updates
	// the dimensions used when [acquire] grows the pool.
	void resize_all(int width, int height);

	// Free every palette in O(N). Done at the start of every frame.
	void free_all();

	// Acquire a free palette, returns its id. If no palette is free
	// and the pool size is below [kMaxFbos], logs a warning and grows
	// the pool by one. Returns -1 only when the pool is at the hard
	// cap (or a GL allocation failed); that case is logged at error
	// level.
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
	int cur_w_ = 0;
	int cur_h_ = 0;

	void destroy_all();
	static bool create_fbo(Fbo &out, int width, int height);
};

} // namespace declgl
