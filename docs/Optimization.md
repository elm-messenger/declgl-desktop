# Rendering optimization audit

This document records the current renderer bottlenecks ranked by expected
benefit, with special attention to work that can be moved from per-frame or
per-draw execution into program load/compile time.

| Rank | Issue | Benefit | Load-time possible? | Main location |
|---:|---|---:|---:|---|
| 1 | Static attributes / indices are still uploaded every draw | Very high for quads/textures/effects | Yes | `src/renderer/program_base.cc` |
| 2 | Uniform and attribute locations use string lookups every draw | High | Yes | `src/renderer/program_base.cc`, `src/gpu/program.cc` |
| 3 | Shared streaming VAO forces attribute reconfiguration and cleanup every draw | High | Mostly yes | `src/renderer/program_base.cc` |
| 4 | `DrawState` allocates/copies dynamic data every draw | High for many small draws | Partly | `src/renderer/program_base.h` |
| 5 | `prepare()` repeatedly does linear protobuf field searches | Medium-high | Partly / per-call index cache | `src/renderer/program_base.cc`, program `prepare()` methods |
| 6 | `DynamicProgram` re-resolves mappings and converts values every frame | Medium-high for custom programs | Yes | `src/renderer/programs/dynamic_program.cc` |
| 7 | Registry lookup constructs temporary `std::string` per atomic | Medium | Yes / easy | `src/renderer/decl_program_registry.h` |
| 8 | Texture/effect/compositor builtin texture maps allocate per pass | Medium | Yes / easy | `src/renderer/renderable_walker.cc` |
| 9 | Textbox performs full layout and mesh rebuild every frame | Very high if text is stable | Runtime cache / first-use compile | `src/renderer/programs/textbox_program.cc` |
| 10 | Too many GL state resets/unbinds after each draw | Medium | Yes / state cache | `src/renderer/program_base.cc` |
| 11 | Per-draw `glBufferData` orphaning and `glBufferSubData` remain expensive for tiny draws | Medium-high | Partly | `src/renderer/program_base.cc` |
| 12 | No batching of same-program atomics | Very high for 900 triangles, but larger change | Runtime/tree-level | `src/renderer/renderable_walker.cc` |

## Details

### 1. Static attributes / indices are still uploaded every draw

`DrawState` marks attributes and indices as static, but `ProgramBase::draw()`
streams them through the shared VBO/EBO every time. Quad UVs, fullscreen effect
UVs, rect/circle unit quads, and fixed quad index buffers should be uploaded to
program-owned GPU buffers once and reused.

Recommended fix: cache static attribute VBOs and static EBOs per program. Longer
term, let programs declare static geometry metadata and upload it during
`ProgramBase::compile()`.

### 2. Uniform and attribute locations use string lookups every draw

The GL program enumerates active locations after linking, but the draw path still
passes names and resolves them per draw. `DrawState` should carry compiled
`GLint` locations so the hot path performs no name hashing/string comparison.

Recommended fix: resolve locations when preparing declarative draw state, and
eventually pre-store known builtin locations on each program after compilation.

### 3. Shared streaming VAO forces repeated attribute setup and cleanup

All programs currently use one shared streaming VAO, so each draw must configure
all attributes and then disable/unbind them. A per-program VAO lets attribute
state persist and reduces cross-program churn.

Recommended fix: give each `ProgramBase` instance its own VAO and dynamic
buffers, track enabled attributes per program, and stop blanket disabling and
unbinds after every draw.

### 4. `DrawState` allocates/copies dynamic data every draw

Dynamic protobuf arrays are copied into `std::vector<float>` in `DrawState`.
This dominates many tiny draws.

Recommended fix: reuse `DrawState` storage, introduce small-vector storage, and
where safe upload/convert directly from protobuf arrays into the streaming
buffer.

### 5. `prepare()` repeatedly does linear protobuf field searches

Each `find_field(fields, key)` scans the whole repeated field list. Programs with
many fields, especially textbox, repeat this many times per draw.

Recommended fix: build a small field view once per atomic/program prepare, or add
numeric/interned field IDs to the protocol.

### 6. `DynamicProgram` re-resolves mappings and converts values every frame

Dynamic programs already parse mappings at creation time, but static values,
primitive strings, attribute locations, uniform locations, and static index data
can be compiled further.

Recommended fix: resolve locations and static GPU buffers after link; only touch
runtime fields for dynamic mappings.

### 7. Registry lookup constructs a temporary string per atomic

`DeclProgramRegistry::get(std::string_view)` currently constructs a temporary
`std::string` for `unordered_map::find`.

Recommended fix: use transparent hashing/equality or a non-allocating lookup
strategy.

### 8. Builtin texture maps allocate per pass

Palette/effect/compositor paths allocate tiny `unordered_map<string, GLuint>`
instances for fixed names like `fbo`, `texture`, `t1`, and `t2`.

Recommended fix: replace the map with a fixed `BuiltinTextures` struct.

### 9. Textbox performs full layout and mesh rebuild every frame

Textbox runs font resolution, kerning, wrapping, alignment, and quad/index buffer
generation on every draw.

Recommended fix: cache textbox layout/mesh by text, font list, sizing, spacing,
alignment, wrapping, and atlas identity/version.

### 10. Too many GL state resets/unbinds after each draw

The renderer owns its GL state but still defensively disables attributes and
unbinds VAO/VBO/EBO after every draw.

Recommended fix: use renderer-owned state caching and leave program VAOs/buffers
bound until another draw needs a different binding.

### 11. Per-draw buffer orphaning remains expensive for tiny draws

Persistent buffers are better than per-draw `glGen`/`glDelete`, but orphaning and
submitting many tiny `glBufferSubData` calls remains costly.

Recommended fix: use a larger ring buffer or persistent mapped streaming buffer.

### 12. No batching of same-program atomics

The walker batches consecutive atomics into the same FBO, but still issues one
program draw per atomic. For the 900-triangle stress test, draw-call count is a
major remaining ceiling.

Recommended fix: add optional program batch hooks and a batched triangle path
that converts per-atomic color uniforms into per-vertex color attributes or a
batched shader variant.
