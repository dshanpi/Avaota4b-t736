#!/usr/bin/env python3
"""Export and replay the display port against its read-only Tina source."""
import difflib
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path('/home/ubuntu/t736-mainline')
TINA = Path('/home/ubuntu/t736-aiot/tina')
KERNEL = ROOT / 'sources/work/linux'
PREFIX = Path('drivers/gpu/drm/t736-bsp')
record = json.loads((ROOT / 'evidence/display-source-import-20261114.json').read_text())
baseline = {}
for name in record['source_files']:
    p = Path(name)
    rel = p.relative_to('bsp/drivers/drm') if name.startswith('bsp/drivers/drm/') else Path('include') / p.relative_to('bsp/include')
    data = (TINA / p).read_text()
    if str(rel) == 'Kconfig':
        data = data.replace('depends on AW_BSP', 'depends on ARCH_SUNXI')
        data = data.replace('depends on DRM && AW_BSP', 'depends on DRM && ARCH_SUNXI')
        data = data.replace('\tselect DRM_KMS_CMA_HELPER\n', '').replace('\tselect DRM_KMS_DMA_HELPER\n', '')
        data = data.replace('drivers/drm/', 'drivers/gpu/drm/t736-bsp/')
    if str(rel) == 'Makefile':
        data += '\nccflags-y += -I$(src)/include -DCONFIG_ARCH_SUN60IW2=1\n'
    baseline[PREFIX / rel] = data
patch = ''.join(''.join(difflib.unified_diff(
    data.splitlines(True), (KERNEL / p).read_text().splitlines(True),
    fromfile='a/' + str(p), tofile='b/' + str(p)))
    for p, data in sorted(baseline.items()))
patch_path = ROOT / 'patches/t736-display-linux618-compat.patch'
patch_path.write_text(patch)
with tempfile.TemporaryDirectory(prefix='t736-display-replay-') as name:
    stage = Path(name)
    for p, data in baseline.items():
        (stage / p).parent.mkdir(parents=True, exist_ok=True)
        (stage / p).write_text(data)
    subprocess.run(['git', 'apply', '--check', str(patch_path)], cwd=stage, check=True)
    subprocess.run(['git', 'apply', str(patch_path)], cwd=stage, check=True)
    for p in baseline:
        assert (stage / p).read_bytes() == (KERNEL / p).read_bytes(), p
print(json.dumps({'display_source_replay_files': len(baseline), 'all_match_compiled_tree': True}))
