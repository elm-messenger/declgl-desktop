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
| CMake       | 4.x (≥ 3.20)   | Build system                                |
| Ninja       | 1.13+          | Generator used by `CMakePresets.json`       |
| vcpkg       | 2026-04-08+    | Manifest mode; toolchain auto-picked        |
| C++ toolchain | Apple Clang 17 / GCC 11+ / clang-cl 18+ (with VS 2022 Build Tools) | C++17 |

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

The Windows build targets the **OCaml 5 `msvc64` switch** + **clang-cl** +
**lld-link**, with the vcpkg `x64-windows-static` triplet, so every C++
dependency (SDL3, protobuf, abseil, ...) is statically linked into
`libdeclgl.a` resolution. Run all commands from a **"x64 Native Tools
Command Prompt for VS 2022"** PowerShell (it sets `INCLUDE`, `LIB`, and
`PATH` for the MSVC toolchain). clang-cl ships with the LLVM Windows
installer and reuses the MSVC headers/libs from VS Build Tools.

```powershell
# One-time setup. Run from an elevated "x64 Native Tools" prompt.
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
winget install --id LLVM.LLVM -e             # provides clang-cl + lld-link
# Visual Studio 2022 Build Tools (C++ workload) — required for the Windows SDK.
winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
    --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

# vcpkg.
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat" -disableMetrics

# Make the toolchain visible (persistent, user-level).
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\vcpkg", "User")
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
$env:Path = "$env:VCPKG_ROOT;$env:Path"
```

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

# Windows (x64 Native Tools Command Prompt for VS 2022, PowerShell)
cmake --preset win-debug
cmake --build --preset win-debug
```

## Linking from OCaml

The OCaml-side dune build picks the build directory up via the
`DECLGL_BUILD_DIR` environment variable; flipping this between presets
re-runs the copy rule in `lib/backend/desktop/dune` without a manual
`dune clean`. All three platforms produce `libdeclgl.a` (the on-disk
format is COFF on Windows, ELF on Linux, Mach-O on macOS — only the
file *name* needs to be portable, which is why we rename the MSVC
output from `declgl.lib` to `libdeclgl.a` in CMake):

```bash
# macOS / Linux
DECLGL_BUILD_DIR=$PWD/declgl-desktop/build/mac-debug   dune build
```

```powershell
# Windows (use the OCaml `msvc64` switch from a "x64 Native Tools" prompt)
opam switch 5.2.0+msvc64
$env:DECLGL_BUILD_DIR = "$PWD\declgl-desktop\build\win-debug"
dune build
```
