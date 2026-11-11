#!/usr/bin/env python3
"""Convert X11 XWD screenshots and 24-bit BMP frame dumps to PNG.

Both formats are produced by the device-side tooling (`xwd -root` for a
screenshot, `VITA2NATIVE_DUMP_FRAME` for a headless frame dump). The image is
written with zlib only, so this runs on any host with a stock python3.

    tools/img2png.py shot.xwd shot.png
    tools/img2png.py frame_0400.bmp frame_0400.png
"""

import struct
import sys
import zlib


def _png(width, height, rgb):
    """Encode an RGB byte string as a PNG (no external dependencies)."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 6)) +
            chunk(b'IEND', b''))


def read_bmp(data):
    """Read the 24-bit bottom-up BMP written by SceDisplay_null.c."""
    offset, = struct.unpack_from('<I', data, 10)
    width, height = struct.unpack_from('<ii', data, 18)
    bpp, = struct.unpack_from('<H', data, 28)
    if bpp != 24:
        raise SystemExit('img2png: unsupported BMP depth %d' % bpp)
    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    out = bytearray(width * height * 3)
    for y in range(height):
        src_y = y if top_down else height - 1 - y
        row = data[offset + src_y * stride:offset + src_y * stride + width * 3]
        if len(row) < width * 3:
            raise SystemExit('img2png: truncated BMP')
        # BMP stores BGR.
        out[y * width * 3:(y + 1) * width * 3] = bytes(
            b for i in range(0, width * 3, 3) for b in (row[i + 2], row[i + 1], row[i]))
    return width, height, bytes(out)


def read_xwd(data):
    """Read a ZPixmap XWD screenshot, honouring the file's byte order."""
    if len(data) < 100:
        raise SystemExit('img2png: truncated XWD header')
    for endian in ('>', '<'):
        header_size, version = struct.unpack_from(endian + 'II', data, 0)
        if version == 7 and 100 <= header_size < len(data):
            break
    else:
        raise SystemExit('img2png: not an XWD file')

    fields = struct.unpack_from(endian + '25I', data, 0)
    pixmap_format = fields[2]
    width, height = fields[4], fields[5]
    byte_order = fields[7]
    bits_per_pixel, bytes_per_line = fields[11], fields[12]
    red_mask, green_mask, blue_mask = fields[14], fields[15], fields[16]
    ncolors = fields[19]
    if pixmap_format != 2:
        raise SystemExit('img2png: only ZPixmap XWD dumps are supported')

    pixel_start = header_size + ncolors * 12
    bytes_per_pixel = (bits_per_pixel + 7) // 8
    if bytes_per_pixel < 3:
        raise SystemExit('img2png: unsupported XWD depth %d' % bits_per_pixel)

    def channel(value, mask):
        if not mask:
            return 0
        shift = (mask & -mask).bit_length() - 1
        peak = mask >> shift
        return ((value & mask) >> shift) * 255 // peak

    out = bytearray(width * height * 3)
    for y in range(height):
        row = data[pixel_start + y * bytes_per_line:
                   pixel_start + y * bytes_per_line + width * bytes_per_pixel]
        if len(row) < width * bytes_per_pixel:
            raise SystemExit('img2png: truncated XWD pixel data')
        for x in range(width):
            px = row[x * bytes_per_pixel:(x + 1) * bytes_per_pixel]
            value = int.from_bytes(px, 'little' if byte_order == 0 else 'big')
            i = (y * width + x) * 3
            out[i:i + 3] = bytes((channel(value, red_mask),
                                  channel(value, green_mask),
                                  channel(value, blue_mask)))
    return width, height, bytes(out)


def main(argv):
    if len(argv) != 3:
        raise SystemExit('usage: img2png.py <input.(xwd|bmp)> <output.png>')
    with open(argv[1], 'rb') as handle:
        data = handle.read()
    if data[:2] == b'BM':
        width, height, rgb = read_bmp(data)
    else:
        width, height, rgb = read_xwd(data)
    with open(argv[2], 'wb') as handle:
        handle.write(_png(width, height, rgb))
    print('%s: %dx%d' % (argv[2], width, height))


if __name__ == '__main__':
    main(sys.argv)
