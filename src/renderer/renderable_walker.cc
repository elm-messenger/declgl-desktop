// renderer/renderable_walker.cc

#include "renderer/renderable_walker.h"

#include <glad/gl.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "gpu/fbo_pool.h"
#include "log/log.h"
#include "renderer/render_context.h"

namespace declgl
{

namespace
{

using mlregl::transport::common::Value;
using mlregl::transport::render::AtomicRenderable;

// Lookup a field by key. Returns nullptr if absent.
const Value *find_field(const AtomicRenderable &a, std::string_view key)
{
	for (const auto &f : a.fields()) {
		if (f.key() == key)
			return f.has_val() ? &f.val() : nullptr;
	}
	return nullptr;
}

void set_clear_color_from_value(const Value *v)
{
	if (!v || v->kind_case() != Value::kNumberArrayValue ||
	    v->number_array_value().values_size() < 4) {
		glClearColor(0.f, 0.f, 0.f, 1.f);
		return;
	}
	const auto &c = v->number_array_value().values();
	glClearColor(static_cast<float>(c[0]), static_cast<float>(c[1]),
		     static_cast<float>(c[2]), static_cast<float>(c[3]));
}

} // namespace

RenderableWalker::~RenderableWalker() = default;

void RenderableWalker::render(const mlregl::transport::render::Renderable &r,
			      const RenderContext &ctx)
{
	// Snapshot the framebuffer the engine had bound on entry. The
	// top-level palette is blitted back to it via the `palette`
	// program, mirroring JS step():
	//   const pid = drawRenderable(gview);
	//   if (pid >= 0) drawPalette({ fbo: fbos[pid] });
	GLint prev_fbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	target_fbo_at_entry_ = prev_fbo;

	// Forward-rendering fast path: no FBO pool means we can't allocate
	// palettes, so just render directly to the bound framebuffer. This
	// is always safe for trees that contain only atomics with no
	// effects — the visible output is identical because there's
	// nothing to ping-pong through.
	if (!ctx.fbos || ctx.fbos->size() == 0) {
		switch (r.kind_case()) {
			using R = mlregl::transport::render::Renderable;
		case R::kAtomic:
			render_atomic(r.atomic(), ctx);
			break;
		case R::kGroup:
			for (const auto &child : r.group().children())
				render(child, ctx);
			break;
		case R::kComposite:
			if (r.composite().has_left())
				render(r.composite().left(), ctx);
			if (r.composite().has_right())
				render(r.composite().right(), ctx);
			break;
		default:
			break;
		}
		return;
	}

	const int pid = draw_renderable(r, ctx);
	if (pid < 0)
		return;

	// Restore the entry framebuffer and blit the result palette via the
	// [palette] passthrough program.
	glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
	glViewport(0, 0, ctx.pixel_w, ctx.pixel_h);
	ProgramBase *pal = decl_programs_.get("palette");
	if (pal) {
		BuiltinTextures tex;
		if (const Fbo *f = ctx.fbos->get(pid)) {
			tex.fbo = f->texture;
		}
		DrawState state;
		if (pal->prepare({}, ctx, tex, state)) {
			pal->draw(state);
		}
	}
	release_pid(pid, ctx);
}

void RenderableWalker::render_atomic(const AtomicRenderable &a,
				     const RenderContext &ctx)
{
	const std::string &prog_name = a.program();

	// Special case: "clear" is not a draw, it's just glClear with a
	// chosen colour (and an optional depth). Mirrors the JS backend
	// `regl.clear`.
	if (prog_name == "clear") {
		set_clear_color_from_value(find_field(a, "color"));
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	ProgramBase *decl_prog = decl_programs_.get(prog_name);
	if (decl_prog) {
		DrawState state;
		static const BuiltinTextures empty_textures;
		if (decl_prog->prepare(a.fields(), ctx, empty_textures,
				       state)) {
			decl_prog->draw(state);
		}
		return;
	}

	DECLGL_LOG_ERROR("no declarative program '{}'", prog_name);
}

void RenderableWalker::release_pid(int pid, const RenderContext &ctx)
{
	if (pid >= 0 && ctx.fbos)
		ctx.fbos->release(pid);
}

void RenderableWalker::bind_fbo(int pid, const RenderContext &ctx)
{
	if (pid < 0 || !ctx.fbos) {
		glBindFramebuffer(GL_FRAMEBUFFER,
				  static_cast<GLuint>(target_fbo_at_entry_));
		glViewport(0, 0, ctx.pixel_w, ctx.pixel_h);
		return;
	}
	const Fbo *f = ctx.fbos->get(pid);
	if (!f)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
	glViewport(0, 0, f->width, f->height);
}

int RenderableWalker::draw_renderable(
	const mlregl::transport::render::Renderable &r,
	const RenderContext &ctx)
{
	using R = mlregl::transport::render::Renderable;
	switch (r.kind_case()) {
	case R::kAtomic: {
		// JS [drawRenderable]: solo atomic gets its own palette,
		// cleared, drawn into.
		const int pid = ctx.fbos->acquire();
		if (pid < 0)
			return -1;
		bind_fbo(pid, ctx);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		render_atomic(r.atomic(), ctx);
		return pid;
	}
	case R::kGroup:
		return draw_group(r.group(), -1, ctx);
	case R::kComposite:
		return draw_composite(r.composite(), ctx);
	default:
		return -1;
	}
}

int RenderableWalker::draw_group(
	const mlregl::transport::render::GroupRenderable &g, int prev_pid,
	const RenderContext &ctx)
{
	if (g.children_size() == 0)
		return prev_pid;

	// Camera scoping: caller-save / callee-restore. This mirrors JS
	// drawGroup which does `let prev_camera = camera; ... camera = prev_camera;`.
	// We use a per-call RenderContext copy for cleanliness.
	RenderContext child_ctx = ctx;
	if (g.has_camera()) {
		const auto &c = g.camera();
		child_ctx.camera = { static_cast<float>(c.x()),
				     static_cast<float>(c.y()),
				     static_cast<float>(c.zoom()),
				     static_cast<float>(c.rotation()) };
	}

	int cur_pid = prev_pid;
	int i = 0;
	const int n = g.children_size();
	using R = mlregl::transport::render::Renderable;
	while (i < n) {
		const auto &c = g.children(i);
		switch (c.kind_case()) {
		case R::kGroup: {
			// Effect-bearing nested groups break the batch and
			// start fresh; effect-free ones inherit our palette.
			const bool nested_has_effects =
				c.group().effects_size() > 0;
			const int sub = draw_group(
				c.group(), nested_has_effects ? -1 : cur_pid,
				child_ctx);
			cur_pid = simple_compose(cur_pid, sub, child_ctx);
			++i;
			break;
		}
		case R::kComposite: {
			const int sub =
				draw_composite(c.composite(), child_ctx);
			cur_pid = simple_compose(cur_pid, sub, child_ctx);
			++i;
			break;
		}
		case R::kAtomic: {
			// Atomic batching: while consecutive children are
			// atomics, draw them all into the same palette to
			// avoid one acquire/release per atomic. This matches
			// JS drawGroup's inner `while (i < cmds.length)` loop.
			const bool fresh_palette = (cur_pid < 0);
			if (fresh_palette) {
				cur_pid = ctx.fbos->acquire();
				if (cur_pid < 0) {
					++i;
					break;
				}
			}
			bind_fbo(cur_pid, child_ctx);
			if (fresh_palette) {
				glClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
			while (i < n) {
				const auto &lc = g.children(i);
				if (lc.kind_case() != R::kAtomic)
					break;
				render_atomic(lc.atomic(), child_ctx);
				++i;
			}
			break;
		}
		default:
			++i;
			break;
		}
	}

	// Apply effects in declaration order. Each one ping-pongs into a
	// fresh palette and frees the previous one. JS:
	//   curPalette = applyEffect(e, curPalette); freePID(curPalette_old);
	for (int ei = 0; ei < g.effects_size(); ++ei) {
		if (cur_pid < 0)
			break; // empty group, nothing to fade
		const int npid =
			apply_effect(g.effects(ei), cur_pid, child_ctx);
		release_pid(cur_pid, ctx);
		cur_pid = npid;
	}

	return cur_pid;
}

int RenderableWalker::draw_composite(
	const mlregl::transport::render::CompositeRenderable &c,
	const RenderContext &ctx)
{
	if (!c.has_compositor())
		return -1;
	const int r1 = c.has_left() ? draw_renderable(c.left(), ctx) : -1;
	const int r2 = c.has_right() ? draw_renderable(c.right(), ctx) : -1;
	if (r1 < 0 && r2 < 0)
		return -1;

	const int npid = ctx.fbos->acquire();
	if (npid < 0) {
		release_pid(r1, ctx);
		release_pid(r2, ctx);
		return -1;
	}

	const auto &comp = c.compositor();
	ProgramBase *decl_prog = decl_programs_.get(comp.program());
	if (!decl_prog) {
		DECLGL_LOG_ERROR(
			"no declarative compositor program '{}'; falling back to "
			"left palette passthrough",
			comp.program());
		// Compositor program missing: best-effort fallback is to flush
		// either half straight to the new palette so something
		// visible shows up. We pick the left half (matches the JS
		// default-compositor behaviour at mode=0).
		bind_fbo(npid, ctx);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (r1 >= 0) {
			ProgramBase *pal = decl_programs_.get("palette");
			if (pal) {
				BuiltinTextures tex;
				if (const Fbo *f = ctx.fbos->get(r1)) {
					tex.fbo = f->texture;
				}
				DrawState state;
				if (pal->prepare(comp.fields(), ctx, tex,
						 state)) {
					pal->draw(state);
				}
			}
		}
		release_pid(r1, ctx);
		release_pid(r2, ctx);
		return npid;
	}

	bind_fbo(npid, ctx);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Build builtin texture slots with t1 and t2
	BuiltinTextures builtin_textures;
	if (r1 >= 0) {
		if (const Fbo *f = ctx.fbos->get(r1)) {
			builtin_textures.t1 = f->texture;
		}
	}
	if (r2 >= 0) {
		if (const Fbo *f = ctx.fbos->get(r2)) {
			builtin_textures.t2 = f->texture;
		}
	}

	DrawState state;
	if (decl_prog->prepare(comp.fields(), ctx, builtin_textures, state)) {
		decl_prog->draw(state);
	}

	release_pid(r1, ctx);
	release_pid(r2, ctx);
	return npid;
}

int RenderableWalker::simple_compose(int old_pid, int new_pid,
				     const RenderContext &ctx)
{
	if (old_pid < 0)
		return new_pid;
	if (new_pid < 0)
		return old_pid;
	if (old_pid == new_pid)
		return old_pid;

	bind_fbo(old_pid, ctx);
	ProgramBase *pal = decl_programs_.get("palette");
	if (pal) {
		BuiltinTextures tex;
		if (const Fbo *f = ctx.fbos->get(new_pid)) {
			tex.fbo = f->texture;
		}
		DrawState state;
		if (pal->prepare({}, ctx, tex, state)) {
			pal->draw(state);
		}
	}
	release_pid(new_pid, ctx);
	return old_pid;
}

int RenderableWalker::apply_effect(const mlregl::transport::render::Effect &e,
				   int src_pid, const RenderContext &ctx)
{
	const int npid = ctx.fbos->acquire();
	if (npid < 0)
		return src_pid; // pool exhausted, drop the effect

	ProgramBase *decl_prog = decl_programs_.get(e.program());
	if (!decl_prog) {
		DECLGL_LOG_ERROR(
			"no declarative effect program '{}'; falling back to "
			"passthrough",
			e.program());
		// Unknown effect program — fall back to a passthrough.
		bind_fbo(npid, ctx);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		ProgramBase *pal = decl_programs_.get("palette");
		if (pal) {
			BuiltinTextures tex;
			if (const Fbo *f = ctx.fbos->get(src_pid)) {
				tex.fbo = f->texture;
			}
			DrawState state;
			if (pal->prepare(e.fields(), ctx, tex, state)) {
				pal->draw(state);
			}
		}
		return npid;
	}

	bind_fbo(npid, ctx);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Build builtin texture slots with source texture
	BuiltinTextures builtin_textures;
	if (src_pid >= 0) {
		if (const Fbo *f = ctx.fbos->get(src_pid)) {
			builtin_textures.texture = f->texture;
		}
	}

	DrawState state;
	if (decl_prog->prepare(e.fields(), ctx, builtin_textures, state)) {
		decl_prog->draw(state);
	}
	return npid;
}

} // namespace declgl
