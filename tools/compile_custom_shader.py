#!/usr/bin/env python3
"""Validate DraStic custom shaders and build their Switch GPU packs.

OpenGL loads .dfx/.dsd sources directly. Vulkan cannot compile GLSL safely on
the Switch, so this tool writes an adjacent <shader>.dfx.nxvk directory with
one SPIR-V vertex/fragment pair per pass. Keep that directory beside the .dfx
when copying the shader tree to sdmc:/switch/drastic/shaders.
With --uam, also emit native Maxwell binaries in <shader>.dfx.nxdk.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

from build_dfx import (ProgramSpec, compile_vulkan, compose_program,
                       key_values, read_text, sections, validate_opengl)
from build_deko3d import compile_program


def shader_paths(arguments: list[Path]) -> list[Path]:
    found: list[Path] = []
    for argument in arguments:
        if argument.is_dir():
            found.extend(path for path in argument.rglob("*")
                         if path.is_file() and path.suffix.lower() == ".dfx")
        elif argument.is_file() and argument.suffix.lower() == ".dfx":
            found.append(argument)
        else:
            raise ValueError(f"not a .dfx file or directory: {argument}")
    return sorted(set(path.resolve() for path in found),
                  key=lambda path: str(path).lower())


def display_name(manifest: Path) -> str:
    option_sections = sections(read_text(manifest), "options")
    if len(option_sections) != 1:
        raise ValueError(f"{manifest}: expected exactly one <options> section")
    names = key_values(option_sections[0], "name")
    if not names or not names[-1] or len(names[-1]) > 95:
        raise ValueError(f"{manifest}: invalid shader name")
    return names[-1]


def compile_manifest(manifest: Path, glslang: Path, uam: Path | None = None) -> None:
    text = read_text(manifest)
    pass_count = len(sections(text, "pass"))
    if not 1 <= pass_count <= 16:
        raise ValueError(f"{manifest}: pass count must be in [1, 16]")
    name = display_name(manifest)
    output = Path(str(manifest) + ".nxvk")
    output.mkdir(parents=True, exist_ok=True)
    native = Path(str(manifest) + ".nxdk")
    if uam:
        native.mkdir(parents=True, exist_ok=True)
        # Write the complete marker last, so interrupted compilation is rejected.
        (native / "pack.info").unlink(missing_ok=True)
        for old in native.glob("pass*.dksh"):
            old.unlink()
    for old in output.glob("pass*.spv"):
        old.unlink()

    for index in range(pass_count):
        spec = ProgramSpec(f"pass{index}", manifest.name, index)
        vertex, fragment, samplers = compose_program(manifest.parent, spec)
        if not 1 <= len(samplers) <= 16:
            raise ValueError(
                f"{manifest}: pass {index + 1} must use 1 to 16 samplers")
        validate_opengl(glslang, output, f"pass{index}", vertex, fragment)
        compile_vulkan(glslang, output, f"pass{index}",
                       vertex, fragment, samplers)
        if uam:
            compile_program(uam, native, f"pass{index}", vertex, fragment, samplers, packed=True)
        (output / f"dfx_pass{index}_vert.bin").replace(
            output / f"pass{index}.vert.spv")
        (output / f"dfx_pass{index}_frag.bin").replace(
            output / f"pass{index}.frag.spv")

    (output / "pack.info").write_text(
        "format=1\n"
        f"name={name}\n"
        f"passes={pass_count}\n",
        encoding="utf-8", newline="\n")
    print(f"{name}: {pass_count} pass(es) -> {output}")
    if uam:
        (native / "pack.info").write_text(
            f"format=1\nname={name}\npasses={pass_count}\n", encoding="utf-8", newline="\n")
        print(f"{name}: {pass_count} native pass(es) -> {native}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build Switch Vulkan and optional native Deko3D shader packs")
    parser.add_argument("paths", nargs="+", type=Path,
                        help=".dfx files or directories to scan recursively")
    parser.add_argument("--glslang", type=Path,
                        help="path to glslangValidator")
    parser.add_argument("--uam", type=Path,
                        help="also compile native Deko3D packs with this uam executable")
    args = parser.parse_args()

    glslang = args.glslang
    if glslang is None:
        executable = shutil.which("glslangValidator")
        if executable:
            glslang = Path(executable)
    if glslang is None or not glslang.is_file():
        parser.error("glslangValidator was not found; use --glslang PATH")
    if args.uam and not args.uam.is_file():
        parser.error("uam was not found; use --uam PATH")

    try:
        manifests = shader_paths(args.paths)
        if not manifests:
            raise ValueError("no .dfx files were found")
        for manifest in manifests:
            compile_manifest(manifest, glslang.resolve(), args.uam.resolve() if args.uam else None)
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
