// renderer/render_context.h — per-frame draw state.

#pragma once

#include <array>
#include <cmath>

namespace declgl
{

class TextureRegistry;
class FontRegistry;
class FboPool;

struct RenderContext {
	// Virtual-pixel canvas size (set from StartRegl). Used for the `view`
	// uniform of the JS-style vertex shaders.
	float view_w = 0.0f;
	float view_h = 0.0f;

	// Camera as the JS shaders consume it: vec4(cx, cy, zoom, rotation).
	// The default value here is just a placeholder; the engine
	// initializes it to canvas centre (virt_w/2, virt_h/2, 1, 0)
	// immediately after constructing the RenderContext, mirroring
	// ml-regl-js/src/app.js's startup: `camera = [virtWidth/2, virtHeight/2, 1, 0]`.
	std::array<float, 4> camera{ 0.0f, 0.0f, 1.0f, 0.0f };

	// Default-frame target FBO. 0 = system framebuffer.
	unsigned int target_fbo = 0;

	// Pixel viewport (set on resize). Independent of view_*.
	int pixel_w = 0;
	int pixel_h = 0;

	// Letterbox/pillarbox fitted rect, in pixels. The walker blits the
	// final palette into this sub-rect of the system framebuffer so the
	// content keeps the virtual-canvas aspect ratio regardless of window
	// aspect; the surrounding pixels are left as the per-frame clear
	// colour (black) and form the bars. Recomputed in `Engine::render`
	// alongside any pixel_w/pixel_h change. When equal to the full
	// window (window aspect == virtual aspect), bars vanish.
	int fit_off_x = 0;
	int fit_off_y = 0;
	int fit_w = 0;
	int fit_h = 0;

	// M3.D: textures the walker may resolve when an atomic carries a
	// `texture` field. Non-owning — owned by the engine. Null is a
	// valid value (means: no textures registered yet, missing-name
	// lookups all return nullptr at the registry layer).
	const TextureRegistry *textures = nullptr;

	// M3.F: fonts the walker may resolve when an atomic's `program` is
	// `textbox`. Non-owning — owned by the engine. Null causes textbox
	// draws to silently no-op (matches asset-not-yet-loaded behaviour
	// for the JS backend's first-frame race).
	const FontRegistry *fonts = nullptr;

	// M3.E: pool of offscreen palettes the walker uses when rendering
	// [GroupRenderable.effects] / [CompositeRenderable]. Non-owning;
	// owned by the engine. Null = compositing falls back to direct
	// forward rendering (effects/composites silently drop).
	FboPool *fbos = nullptr;
};

// Compute a letterbox/pillarbox fitted rect that preserves the virtual-canvas
// aspect (`virt_w`/`virt_h`) inside an outer rect of `pw`x`ph`. Output
// integers are pixel- or logical-unit-agnostic — the caller decides which
// space `pw`/`ph` are in (engine uses pixels for the GL viewport; the mouse
// bridge uses SDL logical units so it lines up with mouse coords). When the
// outer aspect matches the virtual aspect, off_*=0 and fit_*=p*.
inline void compute_fit_rect(int pw, int ph, double virt_w, double virt_h,
			     int &off_x, int &off_y, int &fit_w, int &fit_h)
{
	if (pw <= 0 || ph <= 0 || virt_w <= 0.0 || virt_h <= 0.0) {
		off_x = 0;
		off_y = 0;
		fit_w = pw > 0 ? pw : 0;
		fit_h = ph > 0 ? ph : 0;
		return;
	}
	const double virt_aspect = virt_w / virt_h;
	const double win_aspect = static_cast<double>(pw) /
				  static_cast<double>(ph);
	if (win_aspect > virt_aspect) {
		// Pillarbox: bars on left/right.
		fit_h = ph;
		fit_w = static_cast<int>(std::lround(ph * virt_aspect));
		if (fit_w > pw) fit_w = pw;
	} else {
		// Letterbox: bars on top/bottom (or exact match).
		fit_w = pw;
		fit_h = static_cast<int>(std::lround(pw / virt_aspect));
		if (fit_h > ph) fit_h = ph;
	}
	off_x = (pw - fit_w) / 2;
	off_y = (ph - fit_h) / 2;
}

} // namespace declgl
