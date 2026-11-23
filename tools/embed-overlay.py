#!/usr/bin/env python3
"""Turn an optional PNG into a C header, with no target-side asset file."""

import argparse
from pathlib import Path
import struct

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--png", type=Path)
args = parser.parse_args()

if args.png:
    data = args.png.read_bytes()
    if (len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or
            data[12:16] != b"IHDR" or struct.unpack(">I", data[8:12])[0] != 13):
        parser.error(f"{args.png} is not a valid PNG image")
    width, height = struct.unpack(">II", data[16:24])
    if not width or not height or width > 4096 or height > 4096:
        parser.error("overlay PNG dimensions must be between 1 and 4096 pixels")
    lines = [", ".join(f"0x{byte:02x}" for byte in data[i:i + 16])
             for i in range(0, len(data), 16)]
    content = ("#define V2N_HAS_OVERLAY_PNG 1\n"
               "static const unsigned char v2n_overlay_png[] = {\n    " +
               ",\n    ".join(lines) + "\n};\n")
else:
    content = "#define V2N_HAS_OVERLAY_PNG 0\n"
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(content)
