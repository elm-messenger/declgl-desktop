// renderer/renderable_walker.cc

#include "renderer/renderable_walker.h"

#include <glad/gl.h>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/fbo_pool.h"
#include "log/log.h"
#include "renderer/render_context.h"
#include "resources/font.h"
#include "resources/font_registry.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl
{

namespace
{

using mlregl::transport::common::Value;
using mlregl::transport::render::AtomicRenderable;
using mlregl::transport::render::ProgramCallField;

// Lookup a field by key. Returns nullptr if absent.
const Value *find_field(const AtomicRenderable &a, std::string_view key)
{
	for (const auto &f : a.fields()) {
		if (f.key() == key)
			return f.has_val() ? &f.val() : nullptr;
	}
	return nullptr;
}

// Coerce a Value to a vector<float>. Number→[n], NumberArray→[..], else {}.
std::vector<float> as_floats(const Value *v)
{
	std::vector<float> out;
	if (!v)
		return out;
	switch (v->kind_case()) {
	case Value::kNumberValue:
		out.push_back(static_cast<float>(v->number_value()));
		break;
	case Value::kNumberArrayValue:
		for (double d : v->number_array_value().values())
			out.push_back(static_cast<float>(d));
		break;
	default:
		break;
	}
	return out;
}

// Map JS-side primitive name strings (as defined in ml-regl-core) to a
// GL primitive enum. Defaults to GL_TRIANGLES for unknown / absent.
GLenum primitive_from_string(const std::string &s)
{
	if (s == "points")
		return GL_POINTS;
	if (s == "lines")
		return GL_LINES;
	if (s == "line strip")
		return GL_LINE_STRIP;
	if (s == "line loop")
		return GL_LINE_LOOP;
	if (s == "triangles")
		return GL_TRIANGLES;
	if (s == "triangle strip")
		return GL_TRIANGLE_STRIP;
	if (s == "triangle fan")
		return GL_TRIANGLE_FAN;
	return GL_TRIANGLES;
}

// Fullscreen NDC unit-quad geometry used by programs that don't ship a
// per-call `pos` field (rect, circle, roundedRect). The exact corners
// depend on the program: rect uses [0..1]² (its vert subtracts 0.5 then
// scales by `posize`), while circle/roundedRect use [-1..1]² so the
// vert can hand world-space coords through `v_position`.
struct QuadGeom {
	std::array<float, 8> verts; // 4 corners, x/y interleaved
	std::array<uint32_t, 6> indices; // two triangles, [0,1,2,0,2,3]
};

const QuadGeom *hardcoded_quad_for(std::string_view program)
{
	static constexpr QuadGeom kRectQuad = {
		// [0,1]² — matches ml-regl-js/src/rect/vert.glsl convention
		{ 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f },
		{ 0u, 1u, 2u, 0u, 2u, 3u },
	};
	static constexpr QuadGeom kNdcQuad = {
		// [-1,1]² — matches ml-regl-js/src/circle/vert.glsl convention
		{ -1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f },
		{ 0u, 1u, 2u, 0u, 2u, 3u },
	};
	if (program == "rect")
		return &kRectQuad;
	if (program == "circle")
		return &kNdcQuad;
	if (program == "roundedRect")
		return &kNdcQuad;
	return nullptr;
}

// True for the four texture-sampling builtins. Captured here so the
// branchy texture path stays out of the hot path for non-textured
// draws (most calls in a typical scene).
bool is_texture_program(std::string_view program)
{
	return program == "texture" || program == "textureCropped" ||
	       program == "centeredTexture" ||
	       program == "centeredCroppedTexture";
}

// True iff this textured program ships its own `texc` attribute as a
// per-call field. The other two get the JS-hardcoded UV layout.
bool texture_program_uses_caller_texc(std::string_view program)
{
	return program == "textureCropped" ||
	       program == "centeredCroppedTexture";
}

// Which programs need the indices-only hardcoded fallback. Both
// textureCropped and texture ship `pos` themselves but reuse the JS
// 6-index quad layout. centeredTexture/centeredCroppedTexture ship
// neither pos nor indices.
const std::array<uint32_t, 6> &texture_quad_indices()
{
	static constexpr std::array<uint32_t, 6> kIdx = {
		0u, 1u, 2u, 0u, 2u, 3u
	};
	return kIdx;
}

// JS-hardcoded `texc` table for [texture] and [centeredTexture]. The
// (Y-flipped) corners drive each quad's UV plumbing in the vertex
// shaders we vendored.
const std::array<float, 8> &texture_default_texc()
{
	static constexpr std::array<float, 8> kTexc = {
		0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f,
	};
	return kTexc;
}

// JS-hardcoded `texc2` table for [centeredCroppedTexture]. Drives the
// per-vertex unit-quad corner used to fan out posize.zw.
const std::array<float, 8> &centered_cropped_texc2()
{
	static constexpr std::array<float, 8> kTexc2 = {
		-0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, -0.5f,
	};
	return kTexc2;
}

// Expand the OCaml's 4-float `texc` (cx, cy, cw, ch) into 8-float
// per-corner UVs. Mirrors the preprocessor in
// ml-regl-js/src/app.js for [centered_texture_cropped]:
//
//     [x1,y1, x1+w,y1, x1+w,y1+h, x1,y1+h]
//
// We do this in the walker because the JS-side closure that does the
// expansion is JS-only — the OCaml builder ships the compact 4-tuple
// directly.
std::array<float, 8> expand_centered_cropped_texc(const std::vector<float> &v)
{
	if (v.size() < 4)
		return texture_default_texc();
	const float x1 = v[0], y1 = v[1], w = v[2], h = v[3];
	return {
		x1, y1, x1 + w, y1, x1 + w, y1 + h, x1, y1 + h,
	};
}

// Set a uniform from a Value, choosing the glUniformNfv flavour from
// the array length. Silently no-ops if the program doesn't have that
// uniform (location < 0).
void set_uniform_from_value(const Program &prog, std::string_view uniform_name,
			    const Value &v)
{
	const GLint loc = prog.uniform_location(uniform_name);
	if (loc < 0)
		return;

	auto vs = as_floats(&v);
	switch (vs.size()) {
	case 1:
		glUniform1f(loc, vs[0]);
		break;
	case 2:
		glUniform2f(loc, vs[0], vs[1]);
		break;
	case 3:
		glUniform3f(loc, vs[0], vs[1], vs[2]);
		break;
	case 4:
		glUniform4f(loc, vs[0], vs[1], vs[2], vs[3]);
		break;
	default: /* scalar non-number, or > 4 — ignore */
		break;
	}
}

} // namespace

RenderableWalker::~RenderableWalker()
{
	// Fullscreen quad resources
	if (fs_ebo_)
		glDeleteBuffers(1, &fs_ebo_);
	if (fs_vbo_)
		glDeleteBuffers(1, &fs_vbo_);
	if (fs_vao_)
		glDeleteVertexArrays(1, &fs_vao_);

	// Streaming resources (reused across atomic draws)
	if (stream_ebo_)
		glDeleteBuffers(1, &stream_ebo_);
	if (stream_vbo_aux2_)
		glDeleteBuffers(1, &stream_vbo_aux2_);
	if (stream_vbo_aux1_)
		glDeleteBuffers(1, &stream_vbo_aux1_);
	if (stream_vbo_pos_)
		glDeleteBuffers(1, &stream_vbo_pos_);
	if (stream_vao_)
		glDeleteVertexArrays(1, &stream_vao_);
}

// Lazily create the shared streaming VAO/VBO/EBO. These are reused across
// every atomic draw to avoid the expensive glGen/glDelete per-call overhead
// that regl avoids by creating buffers at program-definition time.
void RenderableWalker::ensure_streaming_buffers()
{
	if (stream_vao_ != 0)
		return;
	glGenVertexArrays(1, &stream_vao_);
	glGenBuffers(1, &stream_vbo_pos_);
	glGenBuffers(1, &stream_vbo_aux1_);
	glGenBuffers(1, &stream_vbo_aux2_);
	glGenBuffers(1, &stream_ebo_);
}

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
	if (const Program *prog = programs_.get("palette")) {
		glUseProgram(prog->id());
		bind_palette_sampler(*prog, "tex", pid, 0, ctx);
		draw_fullscreen_quad(*prog);
		glBindTexture(GL_TEXTURE_2D, 0);
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
		const auto col = as_floats(find_field(a, "color"));
		if (col.size() >= 4) {
			glClearColor(col[0], col[1], col[2], col[3]);
		} else {
			glClearColor(0.f, 0.f, 0.f, 1.f);
		}
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	// M3.F: textbox follows a fundamentally different geometry source
	// (per-frame layout against an MSDF atlas) than the other primitive
	// / texture programs, so it gets its own branch with custom
	// uniform plumbing. Bail out early before the generic field→uniform
	// loop so `text`, `fonts`, `align`, etc. don't leak into uniform
	// binding.
	if (prog_name == "textbox") {
		render_textbox(a, ctx);
		return;
	}

	const Program *prog = programs_.get(prog_name);
	if (!prog) {
		// Once per missing program, not per frame.
		static std::string last;
		if (last != prog_name) {
			DECLGL_LOG_ERROR("no program '{}'", prog_name);
			last = prog_name;
		}
		return;
	}

	glUseProgram(prog->id());

	// Standard built-in uniforms shared by every JS-style program.
	// Tolerated as absent (some programs don't sample them).
	//
	// The `view` uniform is NOT the raw canvas size: it's
	// `(virtWidth/2, -virtHeight/2)`. Two reasons, both inherited
	// verbatim from ml-regl-js/src/app.js:
	//   1. dividing by half-canvas maps the full canvas to NDC ±1
	//      (the shaders compute `pos = diff * camera.z / view`).
	//   2. negating Y converts screen-space (Y down, origin top-left)
	//      to GL NDC (Y up, origin bottom-left) without any extra
	//      shader logic — same shaders work for both backends.
	if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
		glUniform2f(loc, ctx.view_w * 0.5f, -ctx.view_h * 0.5f);
	}
	if (const GLint loc = prog->uniform_location("camera"); loc >= 0) {
		glUniform4f(loc, ctx.camera[0], ctx.camera[1], ctx.camera[2],
			    ctx.camera[3]);
	}

	// Field-driven uniforms. We try to bind every numeric field as a
	// uniform of the same name on the program; the program object
	// returns -1 for unknown names so unused fields are harmless. This
	// covers `color` (vec4), `posize` (vec4), `angle` (float), `cr`
	// (vec3), `cs` (vec4), `radius` (float), `depth` (float), `alpha`
	// (float), and any future scalars/short vectors a builtin might
	// want.
	//
	// Structural fields are excluded:
	//   - `pos`, `elem`, `prim` are geometry; handled below.
	//   - `texture` is a string sampler binding; resolved later.
	//   - `texc` / `texc2` are vertex attributes, not uniforms.
	bool has_alpha_field = false;
	for (const auto &f : a.fields()) {
		const auto &key = f.key();
		if (!f.has_val())
			continue;
		if (key == "pos" || key == "elem" || key == "prim" ||
		    key == "texture" || key == "texc" || key == "texc2") {
			continue;
		}
		if (key == "alpha")
			has_alpha_field = true;
		set_uniform_from_value(*prog, key, f.val());
	}

	// Default `alpha` = 1.0 if the caller didn't provide it. The JS
	// backend has the same fallback in its preprocessors. Only
	// touched when the program actually has an `alpha` uniform.
	if (!has_alpha_field) {
		if (const GLint loc = prog->uniform_location("alpha");
		    loc >= 0) {
			glUniform1f(loc, 1.0f);
		}
	}

	// ----------------------------------------------------------------
	// Texture path (M3.D).
	//
	// The four sampling builtins all share this routine: bind the
	// named texture to TEXUNIT0, source/synthesize the per-vertex
	// `position`, `texc` and (for the cropped-centered variant) `texc2`
	// streams per the JS app.js setup, then draw the 6-index quad.
	//
	// Kept separate from the general field-driven path because
	// textures need (a) a non-numeric field (`texture` string) which
	// the generic uniform binder can't translate, (b) extra attribute
	// VBOs that vary per-program, and (c) a sampler-uniform
	// assignment (`tex` on TEXUNIT0).
	// ----------------------------------------------------------------
	if (is_texture_program(prog_name)) {
		// 1. Resolve and bind the texture.
		const Value *tex_v = find_field(a, "texture");
		if (!tex_v || tex_v->kind_case() != Value::kStringValue) {
			DECLGL_LOG_ERROR("'{}': missing or non-string "
					 "`texture` field",
					 prog_name);
			return;
		}
		if (!ctx.textures) {
			DECLGL_LOG_ERROR("'{}': no TextureRegistry on "
					 "context; cannot resolve '{}'",
					 prog_name, tex_v->string_value());
			return;
		}
		const Texture *tex = ctx.textures->get(tex_v->string_value());
		if (!tex) {
			// Mirror the JS preprocessor: silently drop draws targeting
			// not-yet-loaded textures. The OCaml side already handles
			// load_texture asynchronously, so this is the normal
			// first-frame condition for any textured asset.
			return;
		}

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex->id());
		if (const GLint loc = prog->uniform_location("tex"); loc >= 0) {
			glUniform1i(loc, 0);
		}

		// 2. Resolve `position` / `texc` / `texc2` source streams.
		std::vector<float> pos_buf;
		const float *positions = nullptr;
		GLsizei vertex_count = 4;

		std::vector<float> texc_buf;
		const float *texc_data = nullptr;
		std::array<float, 8> texc_storage{};

		const float *texc2_data = nullptr;

		if (prog_name == "centeredTexture" ||
		    prog_name == "centeredCroppedTexture") {
			// Vertex shader synthesizes positions from `texc` + `posize`,
			// so there's no `position` attribute to feed.
			positions = nullptr;
			vertex_count = 4;
		} else {
			// `texture` and `textureCropped` source `position` from `pos`.
			pos_buf = as_floats(find_field(a, "pos"));
			if (pos_buf.size() < 8 || pos_buf.size() % 2 != 0) {
				DECLGL_LOG_ERROR(
					"'{}': `pos` must have at "
					"least 4 vertices (got {} floats)",
					prog_name, pos_buf.size());
				return;
			}
			positions = pos_buf.data();
			vertex_count = static_cast<GLsizei>(pos_buf.size() / 2);
		}

		if (texture_program_uses_caller_texc(prog_name)) {
			texc_buf = as_floats(find_field(a, "texc"));
			if (prog_name == "centeredCroppedTexture") {
				// 4-float (cx,cy,cw,ch) → 8-float per-corner expansion.
				texc_storage =
					expand_centered_cropped_texc(texc_buf);
				texc_data = texc_storage.data();
			} else {
				if (texc_buf.size() < 8) {
					DECLGL_LOG_ERROR(
						"'{}': caller-supplied "
						"`texc` must have 8 floats (got {})",
						prog_name, texc_buf.size());
					return;
				}
				texc_data = texc_buf.data();
			}
		} else {
			texc_data = texture_default_texc().data();
		}

		if (prog_name == "centeredCroppedTexture") {
			texc2_data = centered_cropped_texc2().data();
		}

		// 3. Draw using the shared streaming VAO/VBOs.
		// Instead of glGen/glDelete per draw (expensive), we reuse
		// persistent buffers and orphan them via glBufferData.
		// This matches what regl does internally for `regl.prop`.
		ensure_streaming_buffers();
		const auto &idx = texture_quad_indices();

		glBindVertexArray(stream_vao_);

		auto bind_vec2_attrib = [&](const char *name, const float *data,
					    int count, GLuint vbo) {
			const GLint loc = prog->attribute_location(name);
			if (loc < 0 || data == nullptr)
				return;
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER,
				     static_cast<GLsizeiptr>(count * 2 *
							     sizeof(float)),
				     data, GL_STREAM_DRAW);
			glEnableVertexAttribArray(static_cast<GLuint>(loc));
			glVertexAttribPointer(static_cast<GLuint>(loc), 2,
					      GL_FLOAT, GL_FALSE, 0, nullptr);
		};

		bind_vec2_attrib("position", positions, vertex_count,
				 stream_vbo_pos_);
		bind_vec2_attrib("texc", texc_data, vertex_count,
				 stream_vbo_aux1_);
		bind_vec2_attrib("texc2", texc2_data, vertex_count,
				 stream_vbo_aux2_);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_ebo_);
		glBufferData(
			GL_ELEMENT_ARRAY_BUFFER,
			static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)),
			idx.data(), GL_STREAM_DRAW);

		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(idx.size()),
			       GL_UNSIGNED_INT, nullptr);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glBindVertexArray(0);

		glBindTexture(GL_TEXTURE_2D, 0);
		return;
	}

	// Geometry assembly.
	//
	// Three sources of vertex positions, in priority order:
	//   1. an explicit `pos` field (triangle, quad, poly, lines, ...)
	//   2. a hardcoded unit/NDC quad for programs that fix their own
	//      geometry (rect, circle, roundedRect)
	//   3. neither => skip silently.
	const Value *pos_v = find_field(a, "pos");
	std::vector<float> caller_positions = as_floats(pos_v);

	const float *positions = nullptr;
	GLsizei vertex_count = 0;
	const uint32_t *indices = nullptr;
	GLsizei index_count = 0;
	std::vector<uint32_t> caller_indices;

	if (!caller_positions.empty()) {
		if (caller_positions.size() % 2 != 0) {
			DECLGL_LOG_ERROR("'{}': malformed pos field "
					 "({} floats — not even)",
					 prog_name, caller_positions.size());
			return;
		}
		positions = caller_positions.data();
		vertex_count =
			static_cast<GLsizei>(caller_positions.size() / 2);

		// Optional caller-supplied element indices.
		if (const Value *ev = find_field(a, "elem"); ev) {
			const auto efs = as_floats(ev);
			caller_indices.reserve(efs.size());
			for (float e : efs)
				caller_indices.push_back(
					static_cast<uint32_t>(e));
			indices = caller_indices.data();
			index_count =
				static_cast<GLsizei>(caller_indices.size());
		}
	} else if (const QuadGeom *g = hardcoded_quad_for(prog_name); g) {
		positions = g->verts.data();
		vertex_count = 4;
		indices = g->indices.data();
		index_count = 6;
	} else {
		// No geometry at all and no hardcoded fallback — nothing to draw.
		static std::string last;
		if (last != prog_name) {
			DECLGL_LOG_WARN("'{}': no `pos` field and no "
					"hardcoded geometry; skipping",
					prog_name);
			last = prog_name;
		}
		return;
	}

	// Optional primitive-kind override (used by `lines`, `linestrip`,
	// `lineloop`, `function_curve`, ...).
	GLenum prim = GL_TRIANGLES;
	if (const Value *pv = find_field(a, "prim");
	    pv && pv->kind_case() == Value::kStringValue) {
		prim = primitive_from_string(pv->string_value());
	}

	// Use shared streaming VAO/VBO instead of transient per-draw objects.
	// This eliminates the expensive glGen/glDelete per-call overhead.
	ensure_streaming_buffers();

	glBindVertexArray(stream_vao_);
	glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_pos_);
	glBufferData(GL_ARRAY_BUFFER,
		     static_cast<GLsizeiptr>(vertex_count * 2 * sizeof(float)),
		     positions, GL_STREAM_DRAW);

	const GLint pos_loc = prog->attribute_location("position");
	if (pos_loc < 0) {
		DECLGL_LOG_ERROR("'{}': vertex shader has no "
				 "'position' attribute",
				 prog_name);
	} else {
		glEnableVertexAttribArray(static_cast<GLuint>(pos_loc));
		glVertexAttribPointer(static_cast<GLuint>(pos_loc), 2, GL_FLOAT,
				      GL_FALSE, 0, nullptr);

		if (index_count > 0) {
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_ebo_);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER,
				     static_cast<GLsizeiptr>(index_count *
							     sizeof(uint32_t)),
				     indices, GL_STREAM_DRAW);
			glDrawElements(prim, index_count, GL_UNSIGNED_INT,
				       nullptr);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		} else {
			glDrawArrays(prim, 0, vertex_count);
		}

		glDisableVertexAttribArray(static_cast<GLuint>(pos_loc));
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
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

void RenderableWalker::bind_palette_sampler(const Program &prog,
					    std::string_view uniform_name,
					    int pid, int unit,
					    const RenderContext &ctx)
{
	if (!ctx.fbos)
		return;
	const Fbo *f = ctx.fbos->get(pid);
	if (!f)
		return;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, f->texture);
	if (const GLint loc = prog.uniform_location(uniform_name); loc >= 0) {
		glUniform1i(loc, unit);
	}
}

void RenderableWalker::draw_fullscreen_quad(const Program &prog)
{
	// Lazy build: shared across every effect & compositor draw. The
	// attribute layout (texc only) matches what [effect.vert.glsl]
	// expects; the corner ordering matches the JS regl spec
	// (`drawPalette` and friends in app.js):
	//   [1,1,  1,0,  0,0,  0,1]
	if (!fs_built_) {
		static constexpr float kTexc[8] = {
			1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
		};
		static constexpr uint32_t kIdx[6] = { 0, 1, 2, 0, 2, 3 };

		glGenVertexArrays(1, &fs_vao_);
		glGenBuffers(1, &fs_vbo_);
		glGenBuffers(1, &fs_ebo_);

		glBindVertexArray(fs_vao_);
		glBindBuffer(GL_ARRAY_BUFFER, fs_vbo_);
		glBufferData(GL_ARRAY_BUFFER, sizeof(kTexc), kTexc,
			     GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fs_ebo_);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIdx), kIdx,
			     GL_STATIC_DRAW);
		// Note: the `texc` attribute location may differ per program
		// (effect/palette/etc all share effect.vert though, so they
		// tend to match), but glVertexAttribPointer is bound per-VAO
		// by the active attrib *index*, not name. We therefore
		// re-establish the pointer below for whichever program is
		// active, leaving the data in this VAO's VBO unchanged.
		glBindVertexArray(0);
		fs_built_ = true;
	}

	glBindVertexArray(fs_vao_);
	glBindBuffer(GL_ARRAY_BUFFER, fs_vbo_);
	const GLint loc = prog.attribute_location("texc");
	if (loc >= 0) {
		glEnableVertexAttribArray(static_cast<GLuint>(loc));
		glVertexAttribPointer(static_cast<GLuint>(loc), 2, GL_FLOAT,
				      GL_FALSE, 0, nullptr);
	}
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
	if (loc >= 0)
		glDisableVertexAttribArray(static_cast<GLuint>(loc));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
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
	const Program *prog = programs_.get(comp.program());
	if (!prog) {
		// Compositor program missing: best-effort fallback is to flush
		// either half straight to the new palette so something
		// visible shows up. We pick the left half (matches the JS
		// default-compositor behaviour at mode=0).
		bind_fbo(npid, ctx);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (r1 >= 0) {
			if (const Program *pal = programs_.get("palette")) {
				glUseProgram(pal->id());
				bind_palette_sampler(*pal, "tex", r1, 0, ctx);
				draw_fullscreen_quad(*pal);
			}
		}
		release_pid(r1, ctx);
		release_pid(r2, ctx);
		return npid;
	}

	bind_fbo(npid, ctx);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(prog->id());

	// Bind both source palettes as samplers.
	bind_palette_sampler(*prog, "t1", r1, 0, ctx);
	bind_palette_sampler(*prog, "t2", r2, 1, ctx);

	// Numeric/scalar fields → uniforms (e.g. `t` for compFade,
	// `mode` for defaultCompositor).
	for (const auto &f : comp.fields()) {
		if (!f.has_val())
			continue;
		const auto &key = f.key();
		if (key == "t1" || key == "t2")
			continue; // sampler bindings
		// `mode` is an int — special-case integer uniform binding so
		// it doesn't get coerced to float (which a sampler/int uniform
		// wouldn't accept and would log a GL_INVALID_OPERATION).
		if (key == "mode") {
			if (f.val().kind_case() == Value::kNumberValue) {
				if (const GLint loc =
					    prog->uniform_location("mode");
				    loc >= 0) {
					glUniform1i(
						loc,
						static_cast<GLint>(
							f.val().number_value()));
				}
			}
			continue;
		}
		set_uniform_from_value(*prog, key, f.val());
	}

	draw_fullscreen_quad(*prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

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
	if (const Program *prog = programs_.get("palette")) {
		glUseProgram(prog->id());
		bind_palette_sampler(*prog, "tex", new_pid, 0, ctx);
		draw_fullscreen_quad(*prog);
		glBindTexture(GL_TEXTURE_2D, 0);
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

	const Program *prog = programs_.get(e.program());
	if (!prog) {
		// Unknown effect program — fall back to a passthrough.
		bind_fbo(npid, ctx);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (const Program *pal = programs_.get("palette")) {
			glUseProgram(pal->id());
			bind_palette_sampler(*pal, "tex", src_pid, 0, ctx);
			draw_fullscreen_quad(*pal);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		return npid;
	}

	bind_fbo(npid, ctx);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(prog->id());
	bind_palette_sampler(*prog, "tex", src_pid, 0, ctx);

	// Numeric fields → uniforms by name. `view` is provided too
	// (some effects like blur scale by it). The `texture` field key
	// would normally be string and is reserved for the sampler; the
	// walker doesn't treat it specially here because effects don't
	// ship one — the source FBO is auto-bound.
	if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
		glUniform2f(loc, ctx.view_w * 0.5f, -ctx.view_h * 0.5f);
	}
	for (const auto &f : e.fields()) {
		if (!f.has_val())
			continue;
		const auto &key = f.key();
		if (key == "texture" || key == "tex")
			continue;
		set_uniform_from_value(*prog, key, f.val());
	}

	draw_fullscreen_quad(*prog);
	glBindTexture(GL_TEXTURE_2D, 0);
	return npid;
}

// ----------------------------------------------------------------------------
// M3.F: textbox rendering.
//
// Faithful port of ml-regl-js/src/text.js (FontManager.layout +
// populateBuffers). One-shot algorithm, no caching: msdfgen layout is
// cheap enough to recompute every frame and the M3 milestone doesn't
// need the JS LRU/hash cache to hit playable framerates. We can revisit
// once the cost shows up in a profile.
//
// UV math note: this differs from the JS reference because we don't
// upload the atlas with `flipY: true` (the JS engine flips images by
// default; the desktop backend doesn't). The Glyph struct stores
// `v = y/scaleH` (top edge of the glyph in atlas-space, where v grows
// downward), and we pair geometry-top (`y+h` in textbox-local space,
// rendered visually upper after the view's negated Y axis kicks in)
// with the smaller-v UV corner. See [resources/font.cc] for the
// pre-divided UV calculation.
// ----------------------------------------------------------------------------
void RenderableWalker::render_textbox(const AtomicRenderable &a,
				      const RenderContext &ctx)
{
	const Program *prog = programs_.get("textbox");
	if (!prog) {
		static bool warned = false;
		if (!warned) {
			DECLGL_LOG_ERROR("no 'textbox' program registered");
			warned = true;
		}
		return;
	}
	if (!ctx.fonts || !ctx.textures) {
		return; // pre-StartRegl race; silently drop, mirrors JS
	}

	// ---- 1. Pull the option fields ---------------------------------------
	const Value *text_v = find_field(a, "text");
	if (!text_v || text_v->kind_case() != Value::kStringValue) {
		return; // empty text; nothing to draw
	}
	const std::string &text = text_v->string_value();
	if (text.empty())
		return;

	// Resolve fonts: prefer `fonts` (string list) over `font` (single).
	std::vector<std::string> font_names;
	if (const Value *fs = find_field(a, "fonts");
	    fs && fs->kind_case() == Value::kStringArrayValue) {
		for (const auto &s : fs->string_array_value().values()) {
			font_names.push_back(s);
		}
	} else if (const Value *fv = find_field(a, "font");
		   fv && fv->kind_case() == Value::kStringValue) {
		font_names.push_back(fv->string_value());
	}
	if (font_names.empty())
		return;

	// Look up the FontEntry for each font and verify they all share
	// a single atlas texture (mirrors the JS guard).
	std::vector<const Font *> fonts;
	fonts.reserve(font_names.size());
	std::string atlas_key;
	for (const auto &name : font_names) {
		const FontEntry *fe = ctx.fonts->get(name);
		if (!fe || !fe->font) {
			return; // not yet loaded, drop silently
		}
		if (atlas_key.empty()) {
			atlas_key = fe->texture_name;
		} else if (atlas_key != fe->texture_name) {
			static std::string last;
			if (last != name) {
				DECLGL_LOG_ERROR(
					"textbox '{}' mixes fonts with "
					"different atlases ('{}' vs '{}'); using first",
					name, atlas_key, fe->texture_name);
				last = name;
			}
			// Soldier on with the first atlas; this is recoverable.
		}
		fonts.push_back(fe->font.get());
	}
	const Texture *atlas = ctx.textures->get(atlas_key);
	if (!atlas)
		return; // atlas image not loaded yet

	// Numeric options with JS defaults.
	auto field_num = [&](std::string_view key, float fallback) -> float {
		const Value *v = find_field(a, key);
		if (!v || v->kind_case() != Value::kNumberValue)
			return fallback;
		return static_cast<float>(v->number_value());
	};
	auto field_str = [&](std::string_view key,
			     std::string_view fallback) -> std::string {
		const Value *v = find_field(a, key);
		if (!v || v->kind_case() != Value::kStringValue)
			return std::string(fallback);
		return v->string_value();
	};
	auto field_bool = [&](std::string_view key, bool fallback) -> bool {
		const Value *v = find_field(a, key);
		if (!v)
			return fallback;
		if (v->kind_case() == Value::kBoolValue)
			return v->bool_value();
		if (v->kind_case() == Value::kNumberValue)
			return v->number_value() != 0.0;
		return fallback;
	};

	const float size = field_num("size", 24.f);
	const float letter_spacing = field_num("letterSpacing", 0.f);
	const float line_height = field_num("lineHeight", 1.f);
	const float word_spacing = field_num("wordSpacing", 1.f);
	const float tab_size = field_num("tabSize", 4.f);
	const float it = field_num("it", 0.f);
	const float thickness = field_num("thickness", 0.f);
	const float width_limit = field_num("width", 1e30f /*~Infinity*/);
	const std::string align = field_str("align", "left");
	const std::string valign = field_str("valign", "top");
	const bool word_break = field_bool("wordBreak", false);

	const auto color_vec = as_floats(find_field(a, "color"));
	float color[4] = { 1.f, 1.f, 1.f, 1.f };
	if (color_vec.size() >= 4) {
		color[0] = color_vec[0];
		color[1] = color_vec[1];
		color[2] = color_vec[2];
		color[3] = color_vec[3];
	}

	const auto offset_vec = as_floats(find_field(a, "offset"));
	float offset[2] = { 0.f, 0.f };
	if (offset_vec.size() >= 2) {
		offset[0] = offset_vec[0];
		offset[1] = offset_vec[1];
	}

	// ---- 2. Helpers shared by layout + populate ---------------------------
	// Resolve which loaded font owns this codepoint (returns nullptr +
	// glyph=nullptr when the codepoint is in none of the requested fonts).
	auto find_in_fonts = [&](int cp,
				 const Font **out_font) -> const Glyph * {
		for (const Font *f : fonts) {
			if (const Glyph *g = f->find_glyph_by_id(cp)) {
				if (out_font)
					*out_font = f;
				return g;
			}
		}
		if (out_font)
			*out_font = nullptr;
		return nullptr;
	};

	// ---- 3. Layout pass ---------------------------------------------------
	// Produce a list of lines, each carrying the glyphs to draw and
	// the line's pixel width. Mirrors `layout()` in text.js.
	struct LaidGlyph {
		const Glyph *glyph;
		const Font *font;
		float x_in_line;
	};
	struct Line {
		std::vector<LaidGlyph> glyphs;
		float width = 0.f;
	};
	std::vector<Line> lines;
	lines.emplace_back();

	int cursor = 0;
	int word_cursor = 0;
	float word_width = 0.f;
	const Font *prev_glyph_font = nullptr;
	const Glyph *prev_glyph = nullptr;

	auto new_line = [&]() {
		lines.emplace_back();
		word_cursor = cursor;
		word_width = 0.f;
		prev_glyph_font = nullptr;
		prev_glyph = nullptr;
	};

	while (cursor < static_cast<int>(text.size())) {
		const unsigned char ch =
			static_cast<unsigned char>(text[cursor]);

		// Newline: terminate current line.
		if (ch == '\n' || ch == '\r') {
			++cursor;
			new_line();
			continue;
		}

		Line &line = lines.back();
		float advance = 0.f;
		bool is_ws = (ch == ' ' || ch == '\t');

		if (is_ws) {
			word_cursor = cursor + 1;
			word_width = 0.f;
			const Font *fpri = fonts[0];
			const float space_advance =
				static_cast<float>(fpri->space_advance()) *
				size /
				static_cast<float>(fpri->lineHeight() ?
							   fpri->lineHeight() :
							   1);
			if (ch == '\t') {
				advance =
					word_spacing * tab_size * space_advance;
			} else {
				advance = word_spacing * space_advance;
			}
		} else {
			const Font *cf = nullptr;
			const Glyph *g =
				find_in_fonts(static_cast<int>(ch), &cf);
			if (!g) {
				// Character not in any loaded font — drop silently.
				// The JS backend throws here; we choose to be lenient
				// because asset authors routinely omit code points they
				// didn't expect to render.
				++cursor;
				continue;
			}
			// Apply kerning if previous glyph is in same font.
			if (cf == prev_glyph_font && prev_glyph != nullptr) {
				const int kern_amt =
					cf->kerning(prev_glyph->id, g->id);
				const float kern =
					static_cast<float>(kern_amt) * size /
					static_cast<float>(
						cf->lineHeight() ?
							cf->lineHeight() :
							1);
				line.width += kern;
				word_width += kern;
			}
			line.glyphs.push_back({ g, cf, line.width });
			advance = (letter_spacing +
				   static_cast<float>(g->xadvance)) *
				  size /
				  static_cast<float>(cf->lineHeight() ?
							     cf->lineHeight() :
							     1);
			prev_glyph_font = cf;
			prev_glyph = g;
		}

		line.width += advance;
		word_width += advance;

		// Wordwrap: only if width is finite.
		if (line.width > width_limit && width_limit > 0.f) {
			if (is_ws) {
				line.width -= advance;
				++cursor;
				new_line();
				continue;
			}
			if (word_break && line.glyphs.size() > 1) {
				line.width -= advance;
				line.glyphs.pop_back();
				new_line();
				continue;
			} else if (!word_break && word_width != line.width) {
				// Roll back to the start of the current word and put
				// it on a new line. We splice glyphs that started in
				// this word off the line.
				int n_to_remove = cursor - word_cursor + 1;
				if (n_to_remove >
				    static_cast<int>(line.glyphs.size())) {
					n_to_remove = static_cast<int>(
						line.glyphs.size());
				}
				line.glyphs.resize(
					line.glyphs.size() -
					static_cast<size_t>(n_to_remove));
				cursor = word_cursor;
				line.width -= word_width;
				new_line();
				continue;
			}
		}
		++cursor;
	}
	// Drop a trailing empty line so a "foo\n" doesn't render an extra
	// blank line below the text.
	if (lines.back().width == 0.f && lines.back().glyphs.empty()) {
		lines.pop_back();
	}

	// Count drawable glyphs (every laid glyph is drawable; whitespace
	// never enters line.glyphs, see above).
	size_t glyph_count = 0;
	for (const auto &ln : lines)
		glyph_count += ln.glyphs.size();
	if (glyph_count == 0)
		return;

	// ---- 4. Build per-glyph quad buffers ---------------------------------
	std::vector<float> pos_buf; // 4 verts × 2 floats = 8 / glyph
	std::vector<float> uv_buf; // 4 verts × 2 floats = 8 / glyph
	std::vector<uint32_t> idx_buf; // 6 indices / glyph
	pos_buf.reserve(glyph_count * 8);
	uv_buf.reserve(glyph_count * 8);
	idx_buf.reserve(glyph_count * 6);

	// valign baseline. The JS code computes lines.length * size *
	// lineHeight; we mirror that exactly.
	const float total_height =
		static_cast<float>(lines.size()) * size * line_height;
	float y_pen = 0.f;
	if (valign == "center") {
		y_pen = -total_height * 0.5f;
	} else if (valign == "bottom") {
		y_pen = -total_height;
	}

	uint32_t emitted_count = 0;
	for (const auto &ln : lines) {
		for (const auto &lg : ln.glyphs) {
			const Glyph *g = lg.glyph;
			const Font *f = lg.font;

			// Per-glyph horizontal pen.
			float x_pen = lg.x_in_line;
			if (align == "center") {
				x_pen -= ln.width * 0.5f;
			} else if (align == "right") {
				x_pen -= ln.width;
			}

			const float scale =
				size /
				static_cast<float>(
					f->lineHeight() ? f->lineHeight() : 1);
			const float x =
				x_pen + static_cast<float>(g->xoffset) * scale;
			const float y_offset =
				static_cast<float>(g->yoffset) * scale;
			const float w = static_cast<float>(g->width) * scale;
			const float h = static_cast<float>(g->height) * scale;

			// Glyph-local y baseline (per-line pen, NOT cumulative
			// through the inner loop — matches JS's `oldy = y;
			// y += yoffset; ... ; y = oldy;` reset).
			const float gy = y_pen + y_offset;

			// Position quad: TL, BL, TR, BR. JS layout is
			//   [x, y+h, x, y, x+w, y+h, x+w, y]
			// optionally skewed by `it*scale` on top corners.
			const float skew = it * scale;
			pos_buf.push_back(x + skew);
			pos_buf.push_back(gy + h); // TL
			pos_buf.push_back(x);
			pos_buf.push_back(gy); // BL
			pos_buf.push_back(x + w + skew);
			pos_buf.push_back(gy + h); // TR
			pos_buf.push_back(x + w);
			pos_buf.push_back(gy); // BR

			// UV quad — pair each geometry vertex with the matching
			// glyph edge in the *visually* rendered atlas. Note that
			// the textbox-local Y axis grows downward (JS does
			// `y += size * lineHeight` to advance lines), and the
			// `view = (vw/2, -vh/2)` divisor in the vertex shader
			// negates Y to convert to GL NDC (Y up). Net effect on
			// screen:
			//   - the vertex at textbox-local (x, y+h)   ends up at
			//     screen BOTTOM  → must sample glyph BOTTOM → v + vh
			//   - the vertex at textbox-local (x, y)     ends up at
			//     screen TOP     → must sample glyph TOP    → v
			// Getting this wrong flips every glyph upside-down even
			// though string layout and atlas indexing both look right
			// (which is exactly what the M3.F.1 first pass did).
			const float u = g->u;
			const float uw = g->uw;
			const float v = g->v; // top edge of glyph in atlas
			const float vh = g->vh;
			// Vertex order matches the position quad above:
			//   v0 = (x,   y+h)  → screen bottom → v + vh
			//   v1 = (x,   y)    → screen top    → v
			//   v2 = (x+w, y+h)  → screen bottom → v + vh
			//   v3 = (x+w, y)    → screen top    → v
			uv_buf.push_back(u);
			uv_buf.push_back(v + vh); // v0
			uv_buf.push_back(u);
			uv_buf.push_back(v); // v1
			uv_buf.push_back(u + uw);
			uv_buf.push_back(v + vh); // v2
			uv_buf.push_back(u + uw);
			uv_buf.push_back(v); // v3

			// Indices: same triangulation JS uses.
			//   [i*4, i*4+2, i*4+1,  i*4+1, i*4+2, i*4+3]
			const uint32_t b = emitted_count * 4;
			idx_buf.push_back(b);
			idx_buf.push_back(b + 2);
			idx_buf.push_back(b + 1);
			idx_buf.push_back(b + 1);
			idx_buf.push_back(b + 2);
			idx_buf.push_back(b + 3);
			++emitted_count;
		}
		y_pen += size * line_height;
	}

	// ---- 5. Issue the draw -----------------------------------------------
	glUseProgram(prog->id());

	// Standard preamble — matches render_atomic's view/camera path.
	if (const GLint loc = prog->uniform_location("view"); loc >= 0) {
		glUniform2f(loc, ctx.view_w * 0.5f, -ctx.view_h * 0.5f);
	}
	if (const GLint loc = prog->uniform_location("camera"); loc >= 0) {
		glUniform4f(loc, ctx.camera[0], ctx.camera[1], ctx.camera[2],
			    ctx.camera[3]);
	}
	if (const GLint loc = prog->uniform_location("offset"); loc >= 0) {
		glUniform2f(loc, offset[0], offset[1]);
	}
	if (const GLint loc = prog->uniform_location("color"); loc >= 0) {
		glUniform4f(loc, color[0], color[1], color[2], color[3]);
	}
	if (const GLint loc = prog->uniform_location("thickness"); loc >= 0) {
		glUniform1f(loc, thickness);
	}
	// unitRange comes from the *primary* font (JS uses fonts[0]).
	if (const GLint loc = prog->uniform_location("unitRange"); loc >= 0) {
		const Font *fpri = fonts[0];
		glUniform2f(loc, fpri->unit_range_x(), fpri->unit_range_y());
	}
	// Bind atlas → tMap on TEXUNIT0.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas->id());
	if (const GLint loc = prog->uniform_location("tMap"); loc >= 0) {
		glUniform1i(loc, 0);
	}

	// Use shared streaming VAO/VBO instead of transient per-draw objects.
	ensure_streaming_buffers();

	glBindVertexArray(stream_vao_);

	auto bind_vec2 = [&](const char *name, const float *data,
			     size_t n_floats, GLuint vbo) {
		const GLint loc = prog->attribute_location(name);
		if (loc < 0)
			return;
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER,
			     static_cast<GLsizeiptr>(n_floats * sizeof(float)),
			     data, GL_STREAM_DRAW);
		glEnableVertexAttribArray(static_cast<GLuint>(loc));
		glVertexAttribPointer(static_cast<GLuint>(loc), 2, GL_FLOAT,
				      GL_FALSE, 0, nullptr);
	};
	bind_vec2("position", pos_buf.data(), pos_buf.size(), stream_vbo_pos_);
	bind_vec2("uv", uv_buf.data(), uv_buf.size(), stream_vbo_aux1_);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_ebo_);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
		     static_cast<GLsizeiptr>(idx_buf.size() * sizeof(uint32_t)),
		     idx_buf.data(), GL_STREAM_DRAW);

	glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(idx_buf.size()),
		       GL_UNSIGNED_INT, nullptr);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace declgl
