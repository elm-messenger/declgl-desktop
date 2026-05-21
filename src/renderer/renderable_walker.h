// renderer/renderable_walker.h — recursive interpreter for Renderable trees.
//
// M3.E: full compositing pipeline. The walker now mirrors the JS
// `drawRenderable` / `drawGroup` / `drawComp` / `applyEffect` recursion
// against an [FboPool] of offscreen palettes. Atomic batches share a
// palette between nested group/composite breaks; effects ping-pong
// through fresh palettes; composites materialise both halves into
// sibling palettes and combine them with the chosen compositor program.
//
// The recursion's return value is a palette id (an `int`, -1 = empty);
// the engine flushes the top-level palette to the system framebuffer
// via [palette].

#pragma once

#include <cstdint>
#include <string_view>

#include "transport_render.pb.h"
#include "gpu/program_registry.h"
#include "renderer/render_context.h"

namespace declgl
{

class RenderableWalker {
    public:
	explicit RenderableWalker(const ProgramRegistry &programs)
		: programs_(programs)
	{
	}
	~RenderableWalker();

	// Render the given tree under [ctx] onto the currently-bound
	// framebuffer (typically the system framebuffer = 0). M3.E: when
	// [ctx.fbos] is non-null and the tree contains any group/composite/
	// effects, the walker uses offscreen palettes and finishes by
	// blitting the result to whatever framebuffer was bound at entry.
	void render(const mlregl::transport::render::Renderable &r,
		    const RenderContext &ctx);

    private:
	// Returns palette id, or -1 if the subtree is empty.
	int draw_renderable(const mlregl::transport::render::Renderable &r,
			    const RenderContext &ctx);
	int draw_group(const mlregl::transport::render::GroupRenderable &g,
		       int prev_pid, const RenderContext &ctx);
	int
	draw_composite(const mlregl::transport::render::CompositeRenderable &c,
		       const RenderContext &ctx);

	// Compose `new_pid` over `old_pid` using the [palette] program;
	// frees `new_pid` and returns `old_pid`. If `old_pid == -1`,
	// returns `new_pid` unchanged. Mirrors JS [simpleCompose].
	int simple_compose(int old_pid, int new_pid, const RenderContext &ctx);

	// Apply effect [e] to source palette [src_pid], producing a fresh
	// palette returned by id. Frees nothing. Mirrors JS [applyEffect].
	int apply_effect(const mlregl::transport::render::Effect &e,
			 int src_pid, const RenderContext &ctx);

	// Bind a palette FBO + set viewport to its size. id == -1 binds
	// [target_fbo_at_entry_]. Used to walk the chain.
	void bind_fbo(int pid, const RenderContext &ctx);

	// Draw a fullscreen NDC quad with the unit-quad-corner UVs JS
	// hardcodes for effect/compositor programs. Caller has already
	// glUseProgram'd. Bound texture state must be set up by caller.
	void draw_fullscreen_quad(const Program &prog);

	// M3.B/C/D core: render a single atomic onto the currently-bound
	// framebuffer. Unchanged by M3.E.
	void render_atomic(const mlregl::transport::render::AtomicRenderable &a,
			   const RenderContext &ctx);

	// M3.F: textbox branch. Resolves the atomic's `fonts` field to a
	// [Font] + atlas texture, runs the JS-equivalent layout algorithm
	// to produce a per-glyph quad list, uploads dynamic VBOs, and
	// issues one [textbox] draw call.
	void
	render_textbox(const mlregl::transport::render::AtomicRenderable &a,
		       const RenderContext &ctx);

	// Free an FBO if id >= 0. Convenience wrapper around
	// FboPool::release that null-guards the pool.
	void release_pid(int pid, const RenderContext &ctx);

	// Bind palette `pid`'s color texture to the given sampler uniform
	// on TEXUNIT [unit].
	void bind_palette_sampler(const Program &prog,
				  std::string_view uniform_name, int pid,
				  int unit, const RenderContext &ctx);

	const ProgramRegistry &programs_;

	// Lazy-built fullscreen quad used by every effect / compositor
	// draw. Allocated on first use, destroyed in the destructor.
	unsigned int fs_vao_ = 0;
	unsigned int fs_vbo_ = 0;
	unsigned int fs_ebo_ = 0;
	bool fs_built_ = false;

	// Framebuffer the engine had bound when [render] was called. We
	// restore it before the final `palette` blit.
	int target_fbo_at_entry_ = 0;
};

} // namespace declgl
