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
