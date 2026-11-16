#!/usr/bin/env python3
"""Convert an unmodified 24-bit TrueColor XWD screenshot to a PNG."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
assert not args.destination.exists()
data = args.source.read_bytes()
header = struct.unpack('>25I', data[:100])
(header_size, version, pixmap_format, depth, width, height, xoffset,
 byte_order, bitmap_unit, bitmap_order, bitmap_pad, bits_per_pixel, stride,
 visual_class, red, green, blue, bits_per_rgb, color_entries, colors,
 window_width, window_height, window_x, window_y, border) = header
assert version == 7 and pixmap_format == 2 and depth in (24, 32)
assert bits_per_pixel == 32 and visual_class == 4 and xoffset == 0
assert (red, green, blue) == (0xff0000, 0xff00, 0xff)
assert 0 < width <= 16384 and 0 < height <= 16384
assert byte_order in (0, 1) and stride >= width * 4 and header_size >= 100
offset = header_size + colors * 12
assert len(data) == offset + stride * height
mode = 'BGRX' if byte_order == 0 else 'XRGB'
shot = Image.frombytes('RGB', (width, height), data[offset:], 'raw', mode, stride, 1)
shot.save(args.destination)
print(json.dumps({'source': str(args.source), 'source_sha256': hashlib.sha256(data).hexdigest(),
                  'png': str(args.destination), 'width': width, 'height': height,
                  'png_sha256': hashlib.sha256(args.destination.read_bytes()).hexdigest(),
                  'capture_kind': 'Xorg root-window screenshot, not a physical monitor photo'}))
