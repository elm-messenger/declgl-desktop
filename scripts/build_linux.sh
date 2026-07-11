#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: this script only supports Linux" >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "error: apt-get was not found; install the README dependencies manually for your distro" >&2
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${DECLGL_LINUX_PRESET:-linux-release}"
vcpkg_root="${VCPKG_ROOT:-$HOME/vcpkg}"

required_packages=(
    build-essential
    ca-certificates
    cmake
    autoconf
    autoconf-archive
    automake
    curl
    git
    libtool
    ninja-build
    pkg-config
    python3
    tar
    unzip
    zip
    ocaml-nox
    libasound2-dev
    libdbus-1-dev
    libdrm-dev
    libegl1-mesa-dev
    libgbm-dev
    libgl1-mesa-dev
    libgles2-mesa-dev
    libibus-1.0-dev
    libpipewire-0.3-dev
    libpulse-dev
    libsndio-dev
    libudev-dev
    libwayland-dev
    libx11-dev
    libxcursor-dev
    libxext-dev
    libxfixes-dev
    libxft-dev
    libxi-dev
    libxinerama-dev
    libxkbcommon-dev
    libxrandr-dev
    libxss-dev
    libxtst-dev
    wayland-protocols
)

optional_packages=(
    libdecor-0-dev
)

if [[ "${EUID}" -eq 0 ]]; then
    sudo_cmd=()
else
    if ! command -v sudo >/dev/null 2>&1; then
        echo "error: sudo was not found; run as root or install sudo" >&2
        exit 1
    fi
    sudo_cmd=(sudo)
fi

installable_optional_packages=()
for package in "${optional_packages[@]}"; do
    if apt-cache show "$package" >/dev/null 2>&1; then
        installable_optional_packages+=("$package")
    else
        echo "info: optional apt package '$package' is not available on this distro release; skipping"
    fi
done

echo "==> Updating apt package lists"
"${sudo_cmd[@]}" apt-get update

echo "==> Installing build dependencies"
"${sudo_cmd[@]}" apt-get install -y --no-install-recommends \
    "${required_packages[@]}" \
    "${installable_optional_packages[@]}"

if [[ ! -d "$vcpkg_root/.git" ]]; then
    echo "==> Cloning vcpkg into $vcpkg_root"
    git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
    echo "==> Bootstrapping vcpkg"
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi

export VCPKG_ROOT="$vcpkg_root"
export PATH="$VCPKG_ROOT:$PATH"

echo "==> Configuring declgl-desktop with preset '$preset'"
cmake --preset "$preset" -S "$repo_root"

echo "==> Building declgl-desktop with preset '$preset'"
cmake --build --preset "$preset"

echo "==> Build complete: $repo_root/build/$preset/libdeclgl.a"
