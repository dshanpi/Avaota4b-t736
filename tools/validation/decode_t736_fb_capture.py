"""Decode a read-only fbdev PPM capture from serial evidence into a PNG."""
import argparse
import base64
import gzip
import hashlib
import io
import re
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('log', type=Path)
parser.add_argument('png', type=Path)
args = parser.parse_args()
text = args.log.read_text(encoding='utf-8').replace('\r', '')
matches = re.findall(r'\nFB_CAPTURE_BEGIN\n([A-Za-z0-9+/=\n]+)FB_CAPTURE_END', text)
assert len(matches) == 1, len(matches)
ppm = gzip.decompress(base64.b64decode(matches[0]))
assert ppm.startswith(b'P6\n')
im = Image.open(io.BytesIO(ppm))
im.load()
im.save(args.png)
print({'dimensions': im.size, 'png': str(args.png.resolve()),
       'ppm_sha256': hashlib.sha256(ppm).hexdigest()})
