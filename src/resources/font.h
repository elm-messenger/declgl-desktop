#pragma once

// resources/font.h — MSDF font atlas metadata.
//
// One [Font] = one parsed BMFont JSON (msdfgen output). Owns the
// per-glyph metric table + global metrics + (optional) kerning pairs.
// The companion atlas image is stored in the [TextureRegistry] under
// the same name (loaded via [LoadFont.image_url]).
//
// Schema we read (a subset of the full BMFont JSON):
//
//   {
//     "common":        { "scaleW": int, "scaleH": int,
//                        "lineHeight": int, "base": int },
//     "distanceField": { "distanceRange": float },
//     "chars": [
//       { "char": "a", "id": 97, "x": int, "y": int,
//         "width": int, "height": int,
//         "xoffset": int, "yoffset": int, "xadvance": int }, ... ],
//     "kernings": [
//       { "first": int, "second": int, "amount": int }, ...   (optional)
//     ]
//   }
//
// We deliberately ignore `info`, `pages`, `chnl`, page-index, and other
// fields the renderer doesn't need. The walker sources its uniforms
// (unitRange, fontHeight) and per-glyph UV/quad data straight from this
// struct.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace declgl {

// One glyph's atlas-side metrics (already converted to UV space) + the
// engine-side draw metrics. UV math matches the no-flipY case:
//
//   u  = x   / scaleW
//   uw = w   / scaleW
//   v  = y   / scaleH       (top edge of glyph in atlas, V grows down)
//   vh = h   / scaleH
//
// In the walker we then place top corners at (u..u+uw, v) and bottom
// corners at (u..u+uw, v+vh) and pair them with geometry that places
// top corners at higher Y (y+h) — the texture's natural orientation
// composes correctly with the screen-space top-down geometry.
struct Glyph {
    int   id       = 0;     // BMFont character code (Unicode codepoint)
    int   x        = 0;     // atlas-px top-left
    int   y        = 0;
    int   width    = 0;     // atlas-px size
    int   height   = 0;
    int   xoffset  = 0;     // pen-relative draw offset (px @ font size)
    int   yoffset  = 0;
    int   xadvance = 0;     // pen advance after this glyph (px @ font size)

    // Pre-divided UV coordinates so the walker doesn't re-divide each
    // frame. Computed in [Font::parse].
    float u  = 0.f;
    float uw = 0.f;
    float v  = 0.f;
    float vh = 0.f;
};

class Font {
public:
    Font() = default;

    // Parse a BMFont JSON document. [bytes] does not need to be NUL-
    // terminated; we work off (data, size). Returns false on any
    // structural problem (missing common.scaleW, missing chars, ...);
    // [error()] then holds a short human-readable explanation.
    bool parse(const char* bytes, std::size_t size);
    const std::string& error() const { return error_; }

    // Lookup a glyph by character (UTF-8 single-codepoint; we treat
    // ASCII as the common case and walk codepoints byte-wise above
    // 0x7F via [find_glyph_by_id]). Returns nullptr if absent.
    const Glyph* find_glyph(char c) const;
    const Glyph* find_glyph_by_id(int codepoint) const;

    // Kerning offset for the (first, second) pair, or 0 if none.
    // Result is in atlas units (BMFont JSON's `amount` field, which is
    // already in atlas pixels). The walker scales by size/fontHeight.
    int kerning(int first, int second) const;

    // Global metrics — sourced directly from the JSON.
    int   scaleW()        const { return scaleW_; }
    int   scaleH()        const { return scaleH_; }
    int   lineHeight()    const { return lineHeight_; }
    int   base()          const { return base_; }
    float distanceRange() const { return distance_range_; }
    int   glyph_count()   const { return static_cast<int>(glyphs_.size()); }
    int   kerning_count() const { return static_cast<int>(kernings_.size()); }

    // Cached `distanceRange / scaleW` and `distanceRange / scaleH`,
    // i.e. the exact `unitRange` uniform the JS shader expects.
    float unit_range_x() const { return distance_range_ / scaleW_; }
    float unit_range_y() const { return distance_range_ / scaleH_; }

    // The space character's xadvance (in atlas units). If the JSON
    // does not include a space glyph (msdfgen often strips it because
    // it has no rasterised pixels) we fall back to a heuristic so
    // text layout doesn't blow up. The JS reference implementation
    // throws here; we choose to be lenient because asset authors
    // routinely leave space out.
    int space_advance() const { return space_advance_; }

private:
    std::vector<Glyph> glyphs_;
    std::unordered_map<int, std::size_t> by_id_;          // codepoint → idx
    // (first << 21) | second  — packed key for the kerning table.
    std::unordered_map<uint64_t, int> kernings_;

    int   scaleW_         = 0;
    int   scaleH_         = 0;
    int   lineHeight_     = 0;
    int   base_           = 0;
    float distance_range_ = 0.f;
    int   space_advance_  = 0;

    std::string error_;
};

}  // namespace declgl
