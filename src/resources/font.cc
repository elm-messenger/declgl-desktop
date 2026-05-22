// resources/font.cc — BMFont JSON (msdfgen output) parser.

#include "resources/font.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "log/log.h"

namespace declgl
{

namespace
{

// Pack two BMFont character codes (each fits in 21 bits — way more
// than enough for the BMP) into a single 64-bit kerning key. The exact
// bit layout doesn't matter as long as it's reversible and collision-
// free for the inputs we accept.
inline uint64_t kerning_key(int first, int second)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(first)) << 32) |
	       static_cast<uint64_t>(static_cast<uint32_t>(second));
}

} // namespace

bool Font::parse(const char *bytes, std::size_t size)
{
	error_.clear();
	glyphs_.clear();
	by_id_.clear();
	kernings_.clear();
	scaleW_ = scaleH_ = lineHeight_ = base_ = 0;
	distance_range_ = 0.f;
	space_advance_ = 0;

	nlohmann::json doc;
	try {
		doc = nlohmann::json::parse(bytes, bytes + size);
	} catch (const std::exception &e) {
		error_ = std::string("json parse: ") + e.what();
		return false;
	}

	// ---- common ----
	auto common_it = doc.find("common");
	if (common_it == doc.end() || !common_it->is_object()) {
		error_ = "missing 'common' object";
		return false;
	}
	const auto &common = *common_it;
	scaleW_ = common.value("scaleW", 0);
	scaleH_ = common.value("scaleH", 0);
	lineHeight_ = common.value("lineHeight", 0);
	base_ = common.value("base", 0);
	if (scaleW_ <= 0 || scaleH_ <= 0) {
		error_ = "common.scaleW / scaleH must be positive";
		return false;
	}

	// ---- distanceField ----
	if (auto df_it = doc.find("distanceField");
	    df_it != doc.end() && df_it->is_object()) {
		distance_range_ = df_it->value("distanceRange", 4.0f);
	} else {
		// Some BMFont JSONs omit this; default to a sane MSDF range.
		distance_range_ = 4.f;
	}

	// ---- chars ----
	auto chars_it = doc.find("chars");
	if (chars_it == doc.end() || !chars_it->is_array()) {
		error_ = "missing 'chars' array";
		return false;
	}

	glyphs_.reserve(chars_it->size());
	by_id_.reserve(chars_it->size());

	const float fw = static_cast<float>(scaleW_);
	const float fh = static_cast<float>(scaleH_);

	for (const auto &c : *chars_it) {
		if (!c.is_object())
			continue;
		Glyph g{};
		g.id = c.value("id", 0);
		g.x = c.value("x", 0);
		g.y = c.value("y", 0);
		g.width = c.value("width", 0);
		g.height = c.value("height", 0);
		g.xoffset = c.value("xoffset", 0);
		g.yoffset = c.value("yoffset", 0);
		g.xadvance = c.value("xadvance", 0);

		// No-flipY UV math: the texture is uploaded with the PNG's
		// natural orientation (Y grows downward), so the glyph's
		// top-left in atlas space maps to (u, v) where v = y/scaleH.
		// Geometry-side, the walker pairs top-of-glyph (visually
		// upper) with the smaller v and bottom (visually lower)
		// with v + vh. See [renderer/renderable_walker.cc].
		g.u = static_cast<float>(g.x) / fw;
		g.uw = static_cast<float>(g.width) / fw;
		g.v = static_cast<float>(g.y) / fh;
		g.vh = static_cast<float>(g.height) / fh;

		const std::size_t idx = glyphs_.size();
		by_id_[g.id] = idx;
		glyphs_.push_back(g);

		if (g.id == 32 /* space */) {
			space_advance_ = g.xadvance;
		}
	}

	// Lenient fallback: if no space glyph is in the JSON, derive an
	// advance from a representative low-letter (typically 'n') so
	// word spacing doesn't degenerate to zero. The JS reference
	// throws here; we choose to be lenient.
	if (space_advance_ == 0) {
		if (auto it = by_id_.find('n'); it != by_id_.end()) {
			space_advance_ = glyphs_[it->second].xadvance;
		} else if (auto it = by_id_.find('m'); it != by_id_.end()) {
			space_advance_ = glyphs_[it->second].xadvance;
		} else if (lineHeight_ > 0) {
			// Last resort: ~half a lineheight.
			space_advance_ = lineHeight_ / 2;
		} else {
			space_advance_ = 8; // arbitrary positive value
		}
		declgl::log::warn("declgl/font",
				  "no space glyph in atlas; "
				  "falling back to xadvance=%d",
				  space_advance_);
	}

	// ---- kernings (optional) ----
	if (auto kern_it = doc.find("kernings");
	    kern_it != doc.end() && kern_it->is_array()) {
		kernings_.reserve(kern_it->size());
		for (const auto &k : *kern_it) {
			if (!k.is_object())
				continue;
			const int first = k.value("first", 0);
			const int second = k.value("second", 0);
			const int amount = k.value("amount", 0);
			if (first == 0 || second == 0)
				continue;
			kernings_[kerning_key(first, second)] = amount;
		}
	}

	return true;
}

const Glyph *Font::find_glyph(char c) const
{
	return find_glyph_by_id(
		static_cast<int>(static_cast<unsigned char>(c)));
}

const Glyph *Font::find_glyph_by_id(int codepoint) const
{
	auto it = by_id_.find(codepoint);
	if (it == by_id_.end())
		return nullptr;
	return &glyphs_[it->second];
}

int Font::kerning(int first, int second) const
{
	auto it = kernings_.find(kerning_key(first, second));
	return it == kernings_.end() ? 0 : it->second;
}

} // namespace declgl
