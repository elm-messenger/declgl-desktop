// renderer/render_context.h — per-frame draw state.

#pragma once

#include <array>

namespace declgl {

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
    std::array<float, 4> camera{0.0f, 0.0f, 1.0f, 0.0f};

    // Default-frame target FBO. 0 = system framebuffer.
    unsigned int target_fbo = 0;

    // Pixel viewport (set on resize). Independent of view_*.
    int  pixel_w = 0;
    int  pixel_h = 0;

    // M3.D: textures the walker may resolve when an atomic carries a
    // `texture` field. Non-owning — owned by the engine. Null is a
    // valid value (means: no textures registered yet, missing-name
    // lookups all return nullptr at the registry layer).
    const TextureRegistry* textures = nullptr;

    // M3.F: fonts the walker may resolve when an atomic's `program` is
    // `textbox`. Non-owning — owned by the engine. Null causes textbox
    // draws to silently no-op (matches asset-not-yet-loaded behaviour
    // for the JS backend's first-frame race).
    const FontRegistry* fonts = nullptr;

    // M3.E: pool of offscreen palettes the walker uses when rendering
    // [GroupRenderable.effects] / [CompositeRenderable]. Non-owning;
    // owned by the engine. Null = compositing falls back to direct
    // forward rendering (effects/composites silently drop).
    FboPool* fbos = nullptr;
};

}  // namespace declgl

