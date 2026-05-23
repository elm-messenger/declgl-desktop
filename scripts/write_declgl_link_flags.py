#!/usr/bin/env python3
"""Generate the dune c_library_flags S-expression for declgl-desktop."""

from __future__ import annotations

import argparse
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


def absl_link_flags_unix(vcpkg_lib: Path) -> list[str]:
    flags: list[str] = []
    for archive in sorted(vcpkg_lib.glob("libabsl_*.a")):
        stem = archive.stem
        if stem.startswith("lib"):
            stem = stem[3:]
        flags.append(f"-l{stem}")
    return flags


def absl_link_flags_msvc(vcpkg_lib: Path) -> list[str]:
    # MSVC/clang-cl with vcpkg `x64-windows-static` produces `absl_*.lib`
    # (no `lib` prefix). ocamlfind passes c_library_flags through the C
    # driver; clang-cl accepts both `-lfoo` (resolves to `foo.lib`) and a
    # bare `foo.lib`. We use `-l<stem>` for symmetry with the *nix paths.
    flags: list[str] = []
    for archive in sorted(vcpkg_lib.glob("absl_*.lib")):
        flags.append(f"-l{archive.stem}")
    return flags


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
    # SDL3, abseil and protobuf reach into Win32 + DirectX. clang-cl picks
    # these up from the Windows SDK on the default lib search path; we just
    # have to name them.
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


def render_lines(vcpkg_lib: Path, build_dir: Path, os_name: str) -> list[str]:
    is_apple = os_name == "Darwin"
    is_windows = os_name == "Windows"

    lines = [
        "(",
        f"  -L{vcpkg_lib}",
        f"  -L{build_dir}",
    ]

    if is_windows:
        # Static vcpkg names: `libprotobuf-lite.lib`, `SDL3-static.lib`,
        # `utf8_range.lib`, `utf8_validity.lib`. `-l` resolves to `<name>.lib`.
        lines.extend(
            [
                "  -llibprotobuf-lite",
                "  -lSDL3-static",
                "  -lutf8_range",
                "  -lutf8_validity",
            ]
        )
        lines.extend(f"  {flag}" for flag in absl_link_flags_msvc(vcpkg_lib))
        lines.extend(f"  {flag}" for flag in windows_system_libs())
    else:
        lines.extend(
            [
                "  -lprotobuf-lite",
                "  -lSDL3",
                "  -lutf8_range",
                "  -lutf8_validity",
            ]
        )
        lines.extend(f"  {flag}" for flag in absl_link_flags_unix(vcpkg_lib))
        if is_apple:
            lines.extend(f"  {flag}" for flag in framework_flags())
        lines.append("  -lstdc++")

    lines.append(")")
    return lines


def main() -> int:
    args = parse_args()
    vcpkg_lib = Path(args.vcpkg_lib).resolve()
    build_dir = Path(args.build_dir).resolve()
    output = Path(args.output).resolve()

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "\n".join(render_lines(vcpkg_lib, build_dir, args.os_name)) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
