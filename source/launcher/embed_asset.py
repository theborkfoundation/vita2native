#!/usr/bin/env python3
"""Embed an asset without requiring a target-side asset directory."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
out = pathlib.Path(sys.argv[2])
name = sys.argv[3]
out.write_text(f'static unsigned char {name}[] = {{\n' +
               ','.join(str(byte) for byte in source) + '\n};\n')
