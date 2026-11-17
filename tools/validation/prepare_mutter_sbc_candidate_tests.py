#!/usr/bin/env python3
"""Extract actual candidate functions for the mocked Cogl/EGL boundary test.

Run after applying the candidate patch to a Mutter 48.7 source tree. This does
not build Mutter, modify its source, or run a display/compositor process.
"""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def section(text, start, end):
    if text.count(start) != 1:
        raise ValueError(f'Candidate section is missing or ambiguous: {start!r}')
    begin = text.index(start)
    finish = text.find(end, begin + len(start))
    if finish < 0:
        raise ValueError(f'Candidate section end is missing: {end!r}')
    return text[begin:finish]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve(strict=True)
    output = args.output_dir.resolve()
    xlib = source_root / 'cogl/cogl/winsys/cogl-onscreen-xlib.c'
    egl = source_root / 'cogl/cogl/winsys/cogl-onscreen-egl.c'
    xlib_text, egl_text = xlib.read_text(encoding='utf-8'), egl.read_text(encoding='utf-8')
    structures = section(xlib_text, 'typedef struct\n{', 'static void\nx11_onscreen_init_iface')
    tracker = section(xlib_text, 'static void\ntrace_sbc_summary',
                      'static gboolean\ncogl_onscreen_xlib_get_window_handles')
    checked = section(egl_text, 'gboolean\ncogl_onscreen_egl_swap_buffers_with_damage_checked',
                      'static void\ncogl_onscreen_egl_swap_buffers_with_damage')
    harness = Path(__file__).resolve().parents[2] / 'tests/mutter_sbc_tracker_harness.c'
    output.mkdir(parents=True, exist_ok=True)
    for name, text in (
        ('mutter_sbc_tracker_fragment.inc', structures + tracker),
        ('mutter_sbc_checked_swap_fragment.inc', checked),
        ('mutter_sbc_tracker_harness.c', harness.read_text(encoding='utf-8')),
    ):
        (output / name).write_text(text, encoding='utf-8', newline='\n')
    manifest = {
        'scope': 'Actual source functions with mocked EGL/Cogl boundaries; not a full Mutter build',
        'inputs': {str(path): sha256(path) for path in (xlib, egl, harness)},
        'generated': {path.name: sha256(path) for path in sorted(output.glob('mutter_sbc_*'))
                      if path.is_file()},
    }
    (output / 'source-bindings.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
