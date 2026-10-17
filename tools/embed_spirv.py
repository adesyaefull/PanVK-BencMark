#!/usr/bin/env python3
"""Embed precompiled SPIR-V shaders into a C header."""

import argparse
import struct
from pathlib import Path


def append_shader(lines: list[str], symbol: str, path: Path) -> None:
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"{path} is not a valid word-aligned SPIR-V file")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if not words or words[0] != 0x07230203:
        raise ValueError(f"{path} does not have a SPIR-V magic word")
    lines.append(f"static const uint32_t {symbol}[] = {{")
    for start in range(0, len(words), 8):
        chunk = ", ".join(f"0x{word:08x}u" for word in words[start:start + 8])
        lines.append(f"    {chunk},")
    lines.append("};")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vertex", required=True, type=Path)
    parser.add_argument("--fragment", required=True, type=Path)
    parser.add_argument("--tess-control", type=Path)
    parser.add_argument("--tess-evaluation", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    lines = [
        "/* Generated from scene shaders with glslangValidator -V. */",
        "#pragma once",
        "#include <stdint.h>",
    ]
    append_shader(lines, "bench_vs_spv", args.vertex)
    append_shader(lines, "bench_fs_spv", args.fragment)
    if args.tess_control:
        append_shader(lines, "bench_tesc_spv", args.tess_control)
    if args.tess_evaluation:
        append_shader(lines, "bench_tese_spv", args.tess_evaluation)
    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
