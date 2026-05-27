// renderer/programs/textbox_program.cc

#include "renderer/programs/textbox_program.h"

#include <array>
#include <string>
#include <vector>

#include "log/log.h"
#include "resources/font.h"
#include "resources/font_registry.h"
#include "resources/texture.h"
#include "resources/texture_registry.h"

namespace declgl
{
namespace programs
{

namespace
{

using mlregl::transport::common::Value;

float field_num(const ProgramCallFields &fields, std::string_view key,
		float fallback)
{
	const ProgramCallField *f = find_field(fields, key);
	if (!f || !f->has_val() ||
	    f->val().kind_case() != Value::kNumberValue) {
		return fallback;
	}
	return static_cast<float>(f->val().number_value());
}

std::string field_str(const ProgramCallFields &fields, std::string_view key,
		      std::string_view fallback)
{
	const ProgramCallField *f = find_field(fields, key);
	if (!f || !f->has_val() ||
	    f->val().kind_case() != Value::kStringValue) {
		return std::string(fallback);
	}
	return f->val().string_value();
}

bool field_bool(const ProgramCallFields &fields, std::string_view key,
		bool fallback)
{
	const ProgramCallField *f = find_field(fields, key);
	if (!f || !f->has_val())
		return fallback;
	if (f->val().kind_case() == Value::kBoolValue)
		return f->val().bool_value();
	if (f->val().kind_case() == Value::kNumberValue)
		return f->val().number_value() != 0.0;
	return fallback;
}

} // namespace

bool TextboxProgram::prepare(const ProgramCallFields &fields,
			     const RenderContext &ctx,
			     const BuiltinTextures & /*builtin_textures*/,
			     DrawState &out_state)
{
	if (!ctx.fonts || !ctx.textures) {
		return false;
	}

	// ---- 1. Pull the option fields ---------------------------------------
	const ProgramCallField *text_f = find_field(fields, "text");
	if (!text_f || !text_f->has_val() ||
	    text_f->val().kind_case() != Value::kStringValue) {
		return false;
	}
	const std::string &text = text_f->val().string_value();
	if (text.empty())
		return false;

	// Resolve fonts: prefer `fonts` (string list) over `font` (single).
	std::vector<std::string> font_names;
	const ProgramCallField *fs_f = find_field(fields, "fonts");
	if (fs_f && fs_f->has_val() &&
	    fs_f->val().kind_case() == Value::kStringArrayValue) {
		for (const auto &s :
		     fs_f->val().string_array_value().values()) {
			font_names.push_back(s);
		}
	} else {
		const ProgramCallField *f_f = find_field(fields, "font");
		if (f_f && f_f->has_val() &&
		    f_f->val().kind_case() == Value::kStringValue) {
			font_names.push_back(f_f->val().string_value());
		}
	}
	if (font_names.empty())
		return false;

	// Look up the FontEntry for each font and verify they all share
	// a single atlas texture (mirrors the JS guard).
	std::vector<const Font *> fonts;
	fonts.reserve(font_names.size());
	std::string atlas_key;
	for (const auto &name : font_names) {
		const FontEntry *fe = ctx.fonts->get(name);
		if (!fe || !fe->font) {
			return false; // not yet loaded, drop silently
		}
		if (atlas_key.empty()) {
			atlas_key = fe->texture_name;
		} else if (atlas_key != fe->texture_name) {
			DECLGL_LOG_ERROR(
				"textbox '{}' mixes fonts with different atlases "
				"('{}' vs '{}'); using first",
				name, atlas_key, fe->texture_name);
			// Soldier on with the first atlas; this is recoverable.
		}
		fonts.push_back(fe->font.get());
	}
	const Texture *atlas = ctx.textures->get(atlas_key);
	if (!atlas)
		return false; // atlas image not loaded yet

	const float size = field_num(fields, "size", 24.f);
	const float letter_spacing = field_num(fields, "letterSpacing", 0.f);
	const float line_height = field_num(fields, "lineHeight", 1.f);
	const float word_spacing = field_num(fields, "wordSpacing", 1.f);
	const float tab_size = field_num(fields, "tabSize", 4.f);
	const float it = field_num(fields, "it", 0.f);
	const float thickness = field_num(fields, "thickness", 0.f);
	const float width_limit = field_num(fields, "width", 1e30f);
	const std::string align = field_str(fields, "align", "left");
	const std::string valign = field_str(fields, "valign", "top");
	const bool word_break = field_bool(fields, "wordBreak", false);

	// Color
	float color[4] = { 1.f, 1.f, 1.f, 1.f };
	const ProgramCallField *col_f = find_field(fields, "color");
	if (col_f && col_f->has_val() &&
	    col_f->val().kind_case() == Value::kNumberArrayValue) {
		const auto &arr = col_f->val().number_array_value().values();
		if (arr.size() >= 4) {
			color[0] = static_cast<float>(arr[0]);
			color[1] = static_cast<float>(arr[1]);
			color[2] = static_cast<float>(arr[2]);
			color[3] = static_cast<float>(arr[3]);
		}
	}

	// Offset
	float offset[2] = { 0.f, 0.f };
	const ProgramCallField *off_f = find_field(fields, "offset");
	if (off_f && off_f->has_val() &&
	    off_f->val().kind_case() == Value::kNumberArrayValue) {
		const auto &arr = off_f->val().number_array_value().values();
		if (arr.size() >= 2) {
			offset[0] = static_cast<float>(arr[0]);
			offset[1] = static_cast<float>(arr[1]);
		}
	}

	// ---- 2. Helpers shared by layout + populate ---------------------------
	// Resolve which loaded font owns this codepoint.
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
				// Roll back to the start of the current word.
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
	// Drop a trailing empty line.
	if (lines.back().width == 0.f && lines.back().glyphs.empty()) {
		lines.pop_back();
	}

	// Count drawable glyphs.
	size_t glyph_count = 0;
	for (const auto &ln : lines)
		glyph_count += ln.glyphs.size();
	if (glyph_count == 0)
		return false;

	// ---- 4. Build per-glyph quad buffers ---------------------------------
	std::vector<float> pos_buf; // 4 verts × 2 floats = 8 / glyph
	std::vector<float> uv_buf; // 4 verts × 2 floats = 8 / glyph
	std::vector<uint32_t> idx_buf; // 6 indices / glyph
	pos_buf.reserve(glyph_count * 8);
	uv_buf.reserve(glyph_count * 8);
	idx_buf.reserve(glyph_count * 6);

	// valign baseline.
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

			// Glyph-local y baseline.
			const float gy = y_pen + y_offset;

			// Position quad: TL, BL, TR, BR.
			const float skew = it * scale;
			pos_buf.push_back(x + skew);
			pos_buf.push_back(gy + h); // TL
			pos_buf.push_back(x);
			pos_buf.push_back(gy); // BL
			pos_buf.push_back(x + w + skew);
			pos_buf.push_back(gy + h); // TR
			pos_buf.push_back(x + w);
			pos_buf.push_back(gy); // BR

			// UV quad.
			const float u = g->u;
			const float uw = g->uw;
			const float v = g->v;
			const float vh = g->vh;
			uv_buf.push_back(u);
			uv_buf.push_back(v + vh); // v0
			uv_buf.push_back(u);
			uv_buf.push_back(v); // v1
			uv_buf.push_back(u + uw);
			uv_buf.push_back(v + vh); // v2
			uv_buf.push_back(u + uw);
			uv_buf.push_back(v); // v3

			// Indices.
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

	// ---- 5. Setup DrawState ----------------------------------------------
	out_state.primitive = GL_TRIANGLES;
	out_state.indexed = true;
	out_state.count = static_cast<GLsizei>(idx_buf.size());

	// Dynamic attributes: position and uv
	add_dyn_attrib(out_state, "position", 2, pos_buf.data(),
		       pos_buf.size() / 2);
	add_dyn_attrib(out_state, "uv", 2, uv_buf.data(), uv_buf.size() / 2);

	// Dynamic indices
	out_state.indices = std::move(idx_buf);

	// Uniforms
	set_builtin_uniforms(ctx, out_state);
	set_uniform_f2(out_state, "offset", offset[0], offset[1]);
	set_uniform_f4(out_state, "color", color[0], color[1], color[2],
		       color[3]);
	set_uniform_f1(out_state, "thickness", thickness);

	// unitRange comes from the primary font.
	const Font *fpri = fonts[0];
	set_uniform_f2(out_state, "unitRange", fpri->unit_range_x(),
		       fpri->unit_range_y());

	// Atlas texture
	set_uniform_tex(out_state, "tMap", atlas->id());

	return true;
}

} // namespace programs
} // namespace declgl
