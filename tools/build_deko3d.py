#!/usr/bin/env python3
"""Compile DraStic post-FX to native Maxwell DKSH; no runtime API translation."""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from build_dfx import PROGRAMS, compose_program, varying_locations, vulkan_stage


def native_stage(source: str, stage: str, samplers: list[str],
                 locations: dict[str, int]) -> str:
    # Reuse the established uniform/varying ABI conversion, then emit ordinary
    # GLSL 450 for uam. SPIR-V/Vulkan are never used by the native backend.
    source = vulkan_stage(source, stage, samplers, locations)
    source = source.replace("layout(push_constant)", "layout(std140, binding = 0)")
    return re.sub(r"layout\(set\s*=\s*0,\s*binding", "layout(binding", source)


def validate_dksh(data: bytes, stage: str) -> None:
    if not 24 <= len(data) <= 8 * 1024 * 1024:
        raise ValueError("invalid DKSH size")
    magic, header, control, code, offset, count = struct.unpack_from("<6I", data)
    if (magic != 0x48534B44 or header != 24 or count != 1 or not code or
            control % 256 or code % 256 or offset < 24 or offset + 64 > control or
            control + code != len(data)):
        raise ValueError("invalid DKSH layout")
    program = struct.unpack_from("<16I", data, offset)
    if (program[0] != (0 if stage == "vert" else 1) or program[1] >= code or
            program[2] > 255 or program[3] + program[4] > code or program[5] > 65536):
        raise ValueError("unsupported DKSH program")


def compile_stage(uam: Path, source: str, stage: str, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="drastic-uam-") as temp:
        shader = Path(temp) / f"shader.{stage}"
        shader.write_text(source, encoding="utf-8", newline="\n")
        result = subprocess.run([str(uam), "-s", stage, "-o", str(output), str(shader)],
                                capture_output=True, text=True)
        if result.returncode:
            raise ValueError(f"{output.name}: uam failed\n{result.stdout}{result.stderr}")
    validate_dksh(output.read_bytes(), stage)


def compile_program(uam: Path, output: Path, name: str, vertex: str,
                    fragment: str, samplers: list[str], packed=False) -> None:
    locations = varying_locations(vertex)
    for kind, stage, source in (("vertex", "vert", vertex), ("fragment", "frag", fragment)):
        path = output / (f"{name}.{stage}.dksh" if packed else f"dfx_{name}_dk_{stage}.bin")
        compile_stage(uam, native_stage(source, kind, samplers, locations), stage, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--uam", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    for spec in PROGRAMS:
        vertex, fragment, samplers = compose_program(args.source, spec)
        compile_program(args.uam.resolve(), output / "data", spec.name,
                        vertex, fragment, samplers)
    root = Path(__file__).resolve().parents[1]
    # The presenter shares the Vulkan backend's already-tested filter math,
    # but is compiled directly to Maxwell code by uam at build time.
    for stage in ("vert", "frag"):
        text = (root / f"source/shaders/drastic_vk.{stage}").read_text(encoding="utf-8")
        text = text.replace("layout(push_constant)", "layout(std140, binding = 0)")
        text = re.sub(r"layout\(set\s*=\s*0,\s*binding", "layout(binding", text)
        # Native source images have exactly one mip level. An explicit base
        # level avoids the legacy negative-bias sampling path for scanlines.
        text = text.replace("texture(source_texture, position, -16.0)",
                            "textureLod(source_texture, position, 0.0)")
        compile_stage(args.uam.resolve(), text, stage, output / "data" / f"dfx_present_dk_{stage}.bin")
    names = [spec.name for spec in PROGRAMS] + ["present"]
    lines = ["/* Generated native Maxwell shaders; inputs supplied by the user's APK. */"]
    for name in names:
        for stage in ("vert", "frag"):
            lines.append(f'#include "dfx_{name}_dk_{stage}_bin.h"')
    lines.append("static const struct { const void *vertex; size_t vertex_size; const void *fragment; size_t fragment_size; } deko_programs[] = {")
    for name in names:
        lines.append(f"  {{dfx_{name}_dk_vert_bin, dfx_{name}_dk_vert_bin_size, dfx_{name}_dk_frag_bin, dfx_{name}_dk_frag_bin_size}},")
    lines.append("};")
    (output / "include").mkdir(parents=True, exist_ok=True)
    (output / "include/deko_programs.h").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"Native Deko3D: {len(names)} vertex/fragment programs compiled and verified")


if __name__ == "__main__":
    main()
