// renderer/render_context.h — per-frame draw state.

#pragma once

#include <array>

namespace declgl {

struct RenderContext {
    // Virtual-pixel canvas size (set from StartRegl). Used for the `view`
    // uniform of the JS-style vertex shaders.
    float view_w = 0.0f;
    float view_h = 0.0f;

    // Camera as the JS shaders consume it: vec4(cx, cy, zoom, rotation).
    std::array<float, 4> camera{0.0f, 0.0f, 1.0f, 0.0f};

    // Default-frame target FBO. 0 = system framebuffer.
    unsigned int target_fbo = 0;

    // Pixel viewport (set on resize). Independent of view_*.
    int  pixel_w = 0;
    int  pixel_h = 0;
};

}  // namespace declgl
