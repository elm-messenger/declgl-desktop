#!/usr/bin/env python3
"""Repack declgl plus selected dependency archives into one static archive."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bundle static dependency archives into libdeclgl.a."
    )
    parser.add_argument("--input", required=True, help="Base declgl archive")
    parser.add_argument("--output", required=True, help="Bundled output archive")
    parser.add_argument("--vcpkg-lib", required=True, help="Path to vcpkg lib dir")
    parser.add_argument("--ar", required=True, help="Archiver executable")
    parser.add_argument("--ranlib", required=True, help="Ranlib executable")
    parser.add_argument(
        "--packages",
        nargs="+",
        default=["protobuf-lite", "sdl3"],
        help="pkg-config packages whose static archives should be bundled",
    )
    return parser.parse_args()


def pkg_config_libs(vcpkg_lib: Path, packages: list[str]) -> list[str]:
    env = os.environ.copy()
    pkg_config_path = str(vcpkg_lib / "pkgconfig")
    existing = env.get("PKG_CONFIG_PATH")
    env["PKG_CONFIG_PATH"] = (
        pkg_config_path if not existing else os.pathsep.join([pkg_config_path, existing])
    )
    output = subprocess.check_output(
        ["pkg-config", "--libs", "--static", *packages],
        encoding="utf-8",
        env=env,
    )
    return shlex.split(output)


def resolve_archive(vcpkg_lib: Path, flag: str) -> Path | None:
    if not flag.startswith("-l"):
        return None
    name = flag[2:]
    if name.startswith(":"):
        candidate = vcpkg_lib / name[1:]
    else:
        candidate = vcpkg_lib / f"lib{name}.a"
    return candidate if candidate.is_file() else None


def append_unique(paths: list[Path], path: Path) -> None:
    resolved = path.resolve()
    if resolved not in paths:
        paths.append(resolved)


def extract_archive(ar: str, archive: Path, destination: Path) -> list[Path]:
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([ar, "x", str(archive)], cwd=destination)
    return sorted(
        path
        for path in destination.iterdir()
        if path.is_file() and not path.name.startswith("__.SYMDEF")
    )


def main() -> int:
    args = parse_args()
    input_archive = Path(args.input).resolve()
    output_archive = Path(args.output).resolve()
    vcpkg_lib = Path(args.vcpkg_lib).resolve()

    archives = [input_archive]
    for flag in pkg_config_libs(vcpkg_lib, args.packages):
        archive = resolve_archive(vcpkg_lib, flag)
        if archive is not None:
            append_unique(archives, archive)

    output_archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="declgl-archive-") as tmp:
        tmpdir = Path(tmp)
        objects: list[Path] = []
        for index, archive in enumerate(archives):
            objects.extend(extract_archive(args.ar, archive, tmpdir / f"archive-{index}"))

        staged_output = tmpdir / output_archive.name
        subprocess.check_call([args.ar, "qc", str(staged_output), *(str(o) for o in objects)])
        subprocess.check_call([args.ranlib, str(staged_output)])
        shutil.move(str(staged_output), output_archive)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
