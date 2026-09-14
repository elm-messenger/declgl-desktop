# AGENTS.md

## Purpose and scope

`declgl-desktop` is the native C++17 host for the OCaml `ml-regl` frontend. It
owns the SDL3 window and input loop, OpenGL 3.3 renderer, asynchronous native
asset loading, SDL audio mixer, persistence/file access, optional control
client, and OCaml callback bridge. Its behavior should match the browser host
in `../ml-regl-js/` for every shared protobuf command.

Keep OCaml description building and diffing in `../lib/`. Keep browser-specific
behavior in `../ml-regl-js/`. This repository decodes and executes the shared
wire contract and must not depend on `ml-messenger` scene/component details.

## Repository map

- `src/runtime/`: transport-neutral SDL/GL loop, frame pacing, protobuf batch
  dispatch, frame capture, and optional JSON control client.
- `src/runtime/loop_hooks.h`: host abstraction used by the OCaml bridge and
  optional standalone player.
- `src/caml_bridge/`: OCaml C stubs and `Callback.register` integration.
- `src/engine/`: backend command dispatch, GL/window ownership, resource
  registries, and audio engine ownership.
- `src/renderer/`: recursive renderable walker, built-in program registry, and
  implementations for drawing, effects, compositors, and textbox rendering.
- `src/gpu/`: GL program helpers and framebuffer pool.
- `src/resources/`: worker-thread asset loader, image decoding, textures, and
  MSDF fonts.
- `src/audio/`: WAV/OGG decoding and the SDL pull-style audio engine.
- `src/log/`: host logging.
- `src/proto_gen/`: CMake rules for generated protobuf C++ sources.
- `shaders/`: built-in GLSL sources embedded into generated build-tree C++.
- `proto/`: shared `declgl-pb` schema checkout.
- `src/elm_host/`, `src/player/`: optional QuickJS/Elm standalone player,
  disabled by default.
- `scripts/`: Linux setup/build, archive/link metadata generation, shader
  embedding, and clang-format helpers.

## Runtime and threading invariants

- The GL/main thread owns SDL window operations, the OpenGL context, command
  dispatch, registry mutation, uploads, and rendering. Never make GL calls on
  an asset or audio worker thread.
- `Runtime` owns the frame loop. Host integrations communicate through
  `LoopHooks`; preserve before-frame, input/update, view, swap/pacing, and
  after-frame ordering.
- OCaml callbacks run on the OCaml runtime thread. Do not call into OCaml from
  the asset worker or SDL audio callback.
- The asset loader has one FIFO worker. It decodes CPU-side data and transfers
  completed results back to the GL thread for upload and event delivery.
  Cancellation must prevent an in-flight load from resurrecting an unloaded
  resource.
- `AudioEngine` has separate GL/main and SDL audio-thread responsibilities.
  The main thread decodes protobuf commands and pushes owned command values;
  the audio thread alone mutates and mixes live voices.
- Audio buffer unload is two-phase. Stop every voice referencing the buffer on
  the audio thread before the command-owned buffer memory is released.
- Audio command timestamps are OCaml-clock milliseconds. Convert them through
  the captured millisecond/frame anchor; do not compare them directly to SDL
  device frames.
- The renderer must restore framebuffer and relevant GL state after recursive
  groups, effects, and composites. Temporary FBOs are pooled and must be
  returned on every path.
- Asset paths are rooted and confined by the runtime asset-root policy. Do not
  weaken traversal checks when adding file operations.
- Control commands are received asynchronously but applied at frame
  boundaries on the render thread.

## Backend parity

Use `../ml-regl-js/src/app.js` and `audio.js` as the behavioral counterpart,
not as code to copy mechanically.

- Preserve shared key names, 1-based mouse buttons, virtual-canvas coordinate
  scaling, texture orientation/filtering, blend behavior, and shader uniform
  meaning.
- Built-in program names and uniform keys must match the OCaml constructors,
  the browser shader directories, and `DeclProgramRegistry`.
- Unsupported commands must produce an explicit error or failure event. Do not
  silently report success.
- Start/stop/volume/timeline/loop/playback-rate audio actions are keyed by
  protobuf `node_group_id` and affect only the addressed voice.
- Success and failure events must be serialized through the shared protobuf
  types with the same units as the browser backend.

## Protocol and generated sources

`proto/` is a checkout of the shared schema repository. Parent OCaml and JS
repositories have sibling checkouts that must stay on the same commit.

- Do not edit generated `.pb.h` or `.pb.cc` files; CMake generates them under
  the selected build directory.
- Prefer additive protobuf evolution. Never reuse a field number or change the
  units/meaning of an existing field.
- Update parsing, dispatch, runtime behavior, event encoding, the OCaml core,
  and browser host together for a shared protocol change.
- Do not edit generated `builtin_shaders.{h,cc}`; change `shaders/*.glsl` or
  `scripts/generate_builtin_shaders.py` and rebuild.

## Build and verification

Set `VCPKG_ROOT` to a bootstrapped vcpkg checkout. Run from this repository
root using the current platform's preset:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Equivalent `mac-debug`, `mac-release`, `linux-release`, `win-debug`, and
`win-release` presets are defined. On Linux, `./scripts/build_linux.sh` can
install system prerequisites, configure, and build, but it changes machine
packages and vcpkg state; do not run it unless that setup work is intended.

The normal build must produce `build/<preset>/libdeclgl.a` and
`build/<preset>/declgl_link_flags.sexp`. Verify OCaml linkage from the parent:

```sh
DECLGL_BUILD_DIR=$PWD/build/linux-debug \
  dune build --root .. test/test_ml_regl_desktop.exe
```

There is no standalone CTest suite currently. Use the smallest parent smoke
application covering the change. Native programs open a real window and may
need a display/audio device; use their bounded quit behavior or control
channel, and do not leave them running. For control behavior, run the parent
`python3 test/native_control_smoke.py` after building `test_fps_smoke_desktop`.
For visual/audio changes, compilation alone is not behavioral verification.

## Style and change discipline

- Follow `.clang-format` for C/C++ and the existing tab-based indentation.
  `scripts/format.sh` rewrites every C/C++ source file, so prefer invoking
  `clang-format -i` only on files in scope unless a full-format change was
  requested.
- Keep ownership explicit with RAII and move-only values. Avoid raw ownership;
  existing raw pointers are borrowed under documented lifetime rules.
- Check protobuf parse results, GL/SDL failures, and missing resource/voice
  lookups. Host failures should be logged or returned, not converted to
  undefined behavior.
- Do not edit `build/`, vcpkg installation trees, generated protobuf sources,
  generated embedded shaders, or vendored `third_party/` code unless the task
  explicitly targets it.
- Do not enable `BUILD_ELM_PLAYER` for routine OCaml backend builds; it adds a
  separate QuickJS host and dependency surface.
- Inspect this repository's own status and diff before finishing. Keep changes
  narrow and follow the existing Conventional Commit subject style when asked
  to commit.
