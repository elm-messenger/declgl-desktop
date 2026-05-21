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
| C++ toolchain | Apple Clang 17 / GCC 11+ / MSVC 2022 | C++17                |

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
```
