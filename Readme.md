# declgl-desktop

A **C++ / SDL3 / OpenGL 3.3** desktop backend for the
[ml-regl](https://github.com/elm-messenger/ml-regl) OCaml
frontend.

It is a drop-in replacement for the JavaScript backend
([`ml-regl-js`](https://github.com/elm-messenger/ml-regl-js)) so that an
ml-regl application written in OCaml can run as a native desktop application
instead of (or in addition to) a web page. Application logic stays in OCaml; this
project owns the window, GL renderer, audio, and input.

## Dependencies

| Tool        | Tested version | Notes                                       |
| ----------- | -------------- | ------------------------------------------- |
| CMake       | 4.x (>= 3.20)  | Build system                                |
| Ninja       | 1.13+          | Generator used by `CMakePresets.json`       |
| vcpkg       | 2026-04-08+    | Manifest mode; toolchain auto-picked        |
| C++ toolchain | Apple Clang 17 / GCC 11+ / MinGW-w64 GCC 11+ | C++17 |

## Setting up the environment

### macOS

```bash
brew install cmake ninja

# Clone vcpkg somewhere stable, bootstrap once.
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg && ./bootstrap-vcpkg.sh -disableMetrics

# Make the toolchain visible to CMake.
# For example, if you use zsh:
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc
echo 'export PATH="$VCPKG_ROOT:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Linux

For apt-based distributions, the one-command setup and build path is:

```bash
./scripts/build_linux.sh
```

The script installs the
system packages needed by vcpkg and SDL3's Linux source build, bootstraps vcpkg
at `$VCPKG_ROOT` or `~/vcpkg`, then runs the `linux-release` CMake preset. To build
another Linux preset:

```bash
DECLGL_LINUX_PRESET=linux-debug ./scripts/build_linux.sh
```

Manual setup uses the same package set:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake autoconf autoconf-archive automake \
    curl git libtool ninja-build pkg-config python3 tar unzip zip ocaml-nox \
    libasound2-dev libdbus-1-dev libdrm-dev libegl1-mesa-dev libgbm-dev \
    libgl1-mesa-dev libgles2-mesa-dev libibus-1.0-dev \
    libpipewire-0.3-dev libpulse-dev libsndio-dev libudev-dev \
    libwayland-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
    libxft-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev libxss-dev \
    libxtst-dev wayland-protocols libdecor-0-dev

git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg && ./bootstrap-vcpkg.sh -disableMetrics

cat <<'EOF' >> ~/.bashrc
export VCPKG_ROOT="$HOME/vcpkg"
export PATH="$VCPKG_ROOT:$PATH"
EOF
source ~/.bashrc
```

### Windows

We use MSYS and mingw64 build system.

First, install MSYS2.

```
winget install MSYS2.MSYS2
```

Add C:\msys64\mingw64\bin to PATH, use pacman to do an update, and install mingw64 toolchain.

Then install OCaml, use winget:

```
winget install Git.Git OCaml.opam
```
When init, select using existing MSYS2 installation (important).

Then install vcpkg:

```
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat" -disableMetrics

# Make the toolchains visible (persistent, user-level).
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\vcpkg", "User")
[Environment]::SetEnvironmentVariable("Path", "C:\msys64\mingw64\bin;$env:USERPROFILE\vcpkg;$env:Path", "User")
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
$env:Path = "C:\msys64\mingw64\bin;$env:VCPKG_ROOT;$env:Path"
```

Currently when installing `ocaml-protoc-plugin` on windows there is one small bug. You can use [this repo](https://github.com/linsyking/ocaml-protoc-plugin/tree/main) for now.

#### GUI App

By default the application will starts a console for displaying debugging information. If you need to remove that, set environment variable `$env:DECLGL_WINDOWS=1` and rebuild this project.

However, you cannot do any `printf` in OCaml side, it will directly kills your app! Remember to clean all the printf in OCaml app before building.

## Building

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

# Windows (PowerShell with MinGW-w64 GCC and vcpkg on PATH)
cmake --preset win-debug
cmake --build --preset win-debug
```

## Linking from OCaml

The OCaml-side dune build picks the build directory up via the
`DECLGL_BUILD_DIR` environment variable; flipping this between presets
re-runs the copy rule in `lib/backend/desktop/dune` without a manual
`dune clean`. All three platforms produce `libdeclgl.a` (the on-disk
format is COFF on Windows, ELF on Linux, Mach-O on macOS):

```bash
# macOS / Linux
DECLGL_BUILD_DIR=$PWD/declgl-desktop/build/mac-debug dune build
```

```powershell
# Windows
$env:DECLGL_BUILD_DIR = "$PWD\declgl-desktop\build\win-debug"
dune build
```

## Running elm-regl applications

The build also produces `declgl-player`, a standalone QuickJS host for
Elm-generated JavaScript. It loads application JavaScript at startup; no Elm
application is embedded in the player binary.

```bash
./build/linux-debug/declgl-player \
  --script /path/to/app.js \
  --asset-root /path/to/project
```

Optional arguments are:

- `--flags flags.json` to pass Elm initialization flags;
- `--module Name` to select the Elm module, defaulting to `Main`;
- `--app-name name` to select the persistence namespace;
- `--frames count` to stop after a bounded number of frames, primarily for
  automated runs.

`Browser.element` applications run against a headless DOM. SDL input is
dispatched through that DOM, but DOM nodes are never displayed. All visible
rendering comes from elm-regl objects sent through `setView`.

The current Elm runtime supports built-in elm-regl programs, groups, effects,
compositors, textures, fonts, and clear commands. Custom shaders
(`createGLProgram`) and save-as-texture render nodes (`_c = 4`) are rejected
with an explicit error. Asset paths are package-relative and confined to
`--asset-root`.

Configure with `-DBUILD_ELM_PLAYER=OFF` to build only the existing OCaml
backend. See [docs/ElmRuntimeDesign.md](docs/ElmRuntimeDesign.md) for the
architecture and compatibility contract.
