#!/usr/bin/env python3
"""Generate the dune c_library_flags S-expression for declgl-desktop."""

from __future__ import annotations

import argparse
import os
import subprocess
import shlex
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Write declgl_link_flags.sexp from resolved build paths."
    )
    parser.add_argument("--vcpkg-lib", required=True, help="Path to vcpkg lib dir")
    parser.add_argument("--build-dir", required=True, help="Path to CMake binary dir")
    parser.add_argument("--output", required=True, help="Path to output .sexp file")
    parser.add_argument(
        "--os-name",
        required=True,
        help="CMake system name, e.g. Darwin, Linux, or Windows",
    )
    return parser.parse_args()


def pkg_config_libs(vcpkg_lib: Path, *packages: str) -> list[str]:
    env = os.environ.copy()
    pkg_config_path = str(vcpkg_lib / "pkgconfig")
    existing = env.get("PKG_CONFIG_PATH")
    env["PKG_CONFIG_PATH"] = (
        pkg_config_path if not existing else os.pathsep.join([pkg_config_path, existing])
    )
    try:
        output = subprocess.check_output(
            ["pkg-config", "--libs", "--static", *packages],
            encoding="utf-8",
            env=env,
        )
    except (OSError, subprocess.CalledProcessError):
        return []
    # SDL's MinGW pkg-config file may include compiler-driver switches such as
    # -mwindows. Dune c_library_flags are routed through FlexDLL, so keep this
    # file to library names and library search paths.
    return [
        flag
        for flag in shlex.split(output)
        if flag.startswith(("-l", "-L"))
    ]


def resolves_to_vcpkg_archive(vcpkg_lib: Path, flag: str) -> bool:
    if not flag.startswith("-l"):
        return False
    name = flag[2:]
    if name.startswith(":"):
        archive = vcpkg_lib / name[1:]
    else:
        archive = vcpkg_lib / f"lib{name}.a"
    return archive.is_file()


def flags_not_bundled_into_archive(vcpkg_lib: Path, flags: list[str]) -> list[str]:
    return [
        flag
        for flag in flags
        if not flag.startswith("-L") and not resolves_to_vcpkg_archive(vcpkg_lib, flag)
    ]


def append_unique(flags: list[str], more_flags: list[str]) -> None:
    seen = set(flags)
    for flag in more_flags:
        if flag not in seen:
            flags.append(flag)
            seen.add(flag)


def framework_flags() -> list[str]:
    frameworks = (
        "Cocoa",
        "IOKit",
        "CoreVideo",
        "CoreAudio",
        "AudioToolbox",
        "CoreHaptics",
        "GameController",
        "Metal",
        "QuartzCore",
        "ForceFeedback",
        "Carbon",
        "UniformTypeIdentifiers",
        "OpenGL",
        "AVFoundation",
        "CoreMedia",
        "CoreFoundation",
        "CoreGraphics",
        "CoreImage",
        "CoreText",
        "CoreServices",
        "CoreBluetooth",
        "Foundation",
    )
    return [f"-framework {name}" for name in frameworks]


def windows_system_libs() -> list[str]:
    # SDL3, abseil and protobuf reach into Win32 + DirectX. The Windows
    # compiler driver picks these up from the default library search path; we
    # just have to name them.
    libs = (
        "user32",
        "gdi32",
        "shell32",
        "advapi32",
        "ole32",
        "oleaut32",
        "uuid",
        "winmm",
        "imm32",
        "version",
        "setupapi",
        "ws2_32",
        "dbghelp",
        "bcrypt",
        "dxgi",
        "dxguid",
        "d3d11",
        "d3d12",
        "shlwapi",
        "userenv",
        "psapi",
    )
    return [f"-l{name}" for name in libs]


def render_lines(
    vcpkg_lib: Path, build_dir: Path, os_name: str
) -> list[str]:
    env = os.environ.copy()
    is_apple = os_name == "Darwin"
    is_windows = os_name == "Windows"

    flags = [
        f"-L{build_dir}",
    ]


    if is_windows:
        if "DECLGL_WINDOWS" in env:
            append_unique(flags, ["-subsystem windows"])
        append_unique(flags, ["-lasmrun"])
    
    slibs = flags_not_bundled_into_archive(
        vcpkg_lib, pkg_config_libs(vcpkg_lib, "protobuf-lite", "sdl3")
    )
    append_unique(flags, slibs)
    if is_apple:
        append_unique(flags, framework_flags())
    if is_windows:
        append_unique(flags, windows_system_libs())
    
    if is_windows:
        append_unique(flags, ["-l:libstdc++.a"])
    else:
        append_unique(flags, ["-lstdc++"])

    return ["(", *(f"  {flag}" for flag in flags), ")"]


def main() -> int:
    args = parse_args()
    vcpkg_lib = Path(args.vcpkg_lib).resolve()
    build_dir = Path(args.build_dir).resolve()
    output = Path(args.output).resolve()

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "\n".join(
            render_lines(vcpkg_lib, build_dir, args.os_name)
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
