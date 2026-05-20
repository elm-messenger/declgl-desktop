# declgl-desktop

A **C++ / SDL3 / OpenGL 3.3** desktop backend for the
[ml-regl](https://github.com/elm-messenger/ml-regl) OCaml game-engine
frontend.

It is a drop-in replacement for the JavaScript backend
([`ml-regl-js`](https://github.com/elm-messenger/ml-regl-js)) so that an
ml-regl game written in OCaml can run as a native desktop application
instead of (or in addition to) a web page. Game logic stays in OCaml; this
project owns the window, GL renderer, audio, and input.

> **Status:** early development. M1 (window + GL ctx) is done; the renderer,
> resource pipeline, audio engine, and OCaml FFI bindings are in progress.
> See [`plan.md`](plan.md) for the milestone roadmap and design notes.

## How it fits with ml-regl

```
+---------------------------- OCaml ---------------------------+
|  ml-regl  (declarative renderable + audio trees, game logic) |
+----- protobuf bytes over a small C ABI (FFI via ctypes) -----+
|  declgl-desktop (this repo)                                  |
|    SDL3 window + GL 3.3 core context                         |
|    GL renderer (built-in programs, FBOs, textures, fonts)    |
|    Declarative audio engine                                  |
|    Input pump                                                |
+--------------------------------------------------------------+
```

The wire protocol is the existing 4 `.proto` files in
[`proto/`](proto/) — identical to what `ml-regl-js` already speaks. No new
protocol is invented here; only a new implementer of the existing one.

Cross-platform target: **macOS, Linux, Windows**. macOS is the current daily
driver.

## Dependencies

Tooling (you install once on your machine):

| Tool        | Tested version | Notes                                       |
| ----------- | -------------- | ------------------------------------------- |
| CMake       | 4.x (≥ 3.20)   | Build system                                |
| Ninja       | 1.13+          | Generator used by `CMakePresets.json`       |
| vcpkg       | 2026-04-08+    | Manifest mode; toolchain auto-picked        |
| C++ toolchain | Apple Clang 17 / GCC 11+ / MSVC 2022 | C++17                |

Libraries (vcpkg installs these per-project from
[`vcpkg.json`](vcpkg.json) — you do not run `vcpkg install` by hand):

- **SDL3** — window, input, audio device.
- **OpenGL 3.3 Core** — rendering, accessed via vendored
  [GLAD2](https://gen.glad.sh/) loader under
  [`third_party/glad/`](third_party/glad/).
- **Protobuf (C++)** — wire format with the OCaml side.
- **glm** — math (mat4 / vec4 / camera transforms).

Vendored single-headers (planned, not yet present): `stb_image`, `dr_wav`,
`dr_mp3`, `stb_vorbis` for image / audio decode in later milestones.

## Setting up the environment

### macOS (one-time)

```bash
brew install cmake ninja

# Clone vcpkg somewhere stable, bootstrap once.
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg && ./bootstrap-vcpkg.sh -disableMetrics

# Make the toolchain visible to CMake.
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc
echo 'export PATH="$VCPKG_ROOT:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Linux (one-time)

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config curl zip unzip tar git \
                        libgl1-mesa-dev libxkbcommon-dev libwayland-dev libxrandr-dev \
                        libxinerama-dev libxcursor-dev libxi-dev libdbus-1-dev

git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg && ./bootstrap-vcpkg.sh -disableMetrics

cat <<'EOF' >> ~/.bashrc
export VCPKG_ROOT="$HOME/vcpkg"
export PATH="$VCPKG_ROOT:$PATH"
EOF
source ~/.bashrc
```

The first `cmake --preset` will build SDL3, protobuf, abseil, glm, and a
few helpers from source under `build/<preset>/vcpkg_installed/`. Expect
**3–5 minutes on a modern machine**. Subsequent configures are instant —
vcpkg caches under `~/vcpkg/buildtrees` and `~/vcpkg/packages`, and the
manifest install becomes a no-op.

## Building

This project ships a [`CMakePresets.json`](CMakePresets.json) so you don't
have to remember toolchain flags.

```bash
# macOS
cmake --preset mac-debug          # configure (slow first time, fast after)
cmake --build --preset mac-debug  # build

# macOS, optimized
cmake --preset mac-release
cmake --build --preset mac-release

# Linux
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Build artifacts land in `build/<preset>/`.

### Running the demo

The M1 smoke test is a tiny standalone executable that opens an SDL3 window
with a GL 3.3 Core context and animates the clear color. It does not yet
talk to OCaml; it just proves the toolchain.

```bash
./build/mac-debug/examples/demo/declgl_demo
```

You should see:

- A 1280×720 window titled `declgl_demo (M1)` with a slowly cycling color.
- A line on stdout like:
  ```
  [declgl_demo] GL 4.1  vendor=Apple  renderer=Apple M2 Pro  glsl=4.10
  ```
- `Esc` or closing the window quits cleanly.

> Note: macOS caps GL at **4.1 Core** even when you ask for 3.3 — that's
> intentional on Apple's side. Our shaders target `#version 330 core`,
> which 4.1 is a strict superset of, so this is fine.

## Repo layout

```
declgl-desktop/
├── CMakeLists.txt              # top-level
├── CMakePresets.json           # mac-debug/-release, linux-debug/-release
├── vcpkg.json                  # manifest: sdl3, protobuf, glm
├── plan.md                     # design notes + milestone roadmap
├── Inst.md                     # original task brief
├── proto/                      # shared wire protocol with ml-regl
│   ├── transport_audio.proto
│   ├── transport_backend.proto
│   ├── transport_common.proto
│   └── transport_render.proto
├── third_party/
│   └── glad/                   # vendored GLAD2 GL 3.3 Core loader
├── examples/
│   └── demo/                   # M1 smoke test
└── (src/ ... arrives in M2-M6)
```

## Workflow tips

- **Re-configure after editing `vcpkg.json`** — that's how vcpkg learns
  about new deps. Most other CMake edits are picked up by `cmake --build`
  automatically.
- **Clean build:** `rm -rf build/<preset>` and re-configure.
- **`compile_commands.json`** is generated next to the build directory
  (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`); point `clangd`/your editor at it
  for accurate IntelliSense.
- **Linker warnings about "object built for newer macOS"** are harmless —
  vcpkg compiles deps against the host SDK; we deploy to macOS 14 in the
  preset.

## License

TBD (matches upstream ml-regl when we settle on it).
