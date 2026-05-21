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
        help="CMake system name, e.g. Darwin or Linux",
    )
    return parser.parse_args()


def absl_link_flags(vcpkg_lib: Path) -> list[str]:
    flags: list[str] = []
    for archive in sorted(vcpkg_lib.glob("libabsl_*.a")):
        stem = archive.stem
        if stem.startswith("lib"):
            stem = stem[3:]
        flags.append(f"-l{stem}")
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


def render_lines(vcpkg_lib: Path, build_dir: Path, os_name: str) -> list[str]:
    absl_flags = absl_link_flags(vcpkg_lib)
    is_apple = os_name == "Darwin"
    lines = [
        "(",
        f"  -L{vcpkg_lib}",
        f"  -L{build_dir}",
        "  -lprotobuf-lite",
        "  -lSDL3",
        "  -lutf8_range",
        "  -lutf8_validity",
    ]

    if is_apple:
        lines.extend(f"  {flag}" for flag in absl_flags)
        lines.extend(f"  {flag}" for flag in framework_flags())
    else:
        lines.append("  -Wl,--start-group")
        lines.extend(f"  {flag}" for flag in absl_flags)
        lines.append("  -Wl,--end-group")

    lines.extend(
        [
            "  -lstdc++",
            ")",
        ]
    )
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
