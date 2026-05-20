// renderer/renderable_walker.h — recursive interpreter for Renderable trees.
//
// The walker takes a decoded protobuf `Renderable` plus a `RenderContext`
// and emits GL draw calls. For now only `AtomicRenderable` is implemented
// (M3.B); `GroupRenderable` and `CompositeRenderable` recursion are
// stubbed with TODO logs that print the structure but defer effects/FBOs
// to M3.E.

#pragma once

#include "transport_render.pb.h"
#include "gpu/program_registry.h"
#include "renderer/render_context.h"

namespace declgl {

class RenderableWalker {
public:
    explicit RenderableWalker(const ProgramRegistry& programs)
        : programs_(programs) {}

    // Render the given tree under [ctx]. The current GL state is assumed
    // to already have the desired framebuffer bound; no swap.
    void render(const mlregl::transport::render::Renderable& r,
                const RenderContext& ctx);

private:
    void render_atomic(const mlregl::transport::render::AtomicRenderable& a,
                       const RenderContext& ctx);
    void render_group(const mlregl::transport::render::GroupRenderable& g,
                      const RenderContext& ctx);
    void render_composite(
        const mlregl::transport::render::CompositeRenderable& c,
        const RenderContext& ctx);

    const ProgramRegistry& programs_;
};

}  // namespace declgl
