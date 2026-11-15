#!/usr/bin/env python3
"""Stage GPL display sources for an isolated Linux 6.18 compatibility build.

This does not change Tina, package firmware, update the board, or claim display
hardware works. Only source/header/build files are imported, with SHA256 records.
"""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path('/home/ubuntu/t736-mainline')
TINA = Path('/home/ubuntu/t736-aiot/tina')
KERNEL = ROOT / 'sources/work/linux'
DEST = KERNEL / 'drivers/gpu/drm/t736-bsp'


def main():
    assert Path(__file__).resolve().parents[1] == ROOT
    assert (KERNEL / 'Makefile').is_file()
    assert not DEST.exists(), 'Do not overwrite a display port already in progress'
    source = TINA / 'bsp/drivers/drm'
    records = {}
    for p in sorted(source.rglob('*')):
        if not p.is_file() or not (p.suffix in ('.c', '.h') or p.name in ('Makefile', 'Kconfig')):
            continue
        relative = p.relative_to(source)
        target = DEST / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        data = p.read_bytes()
        target.write_bytes(data)
        records[str(p.relative_to(TINA))] = hashlib.sha256(data).hexdigest()
    headers = ['sunxi-log.h', 'sunxi-gpio.h', 'sunxi-sid.h', 'sunxi-iommu.h',
               'video/sunxi_drm.h', 'video/sunxi_drm_notify.h',
               'video/sunxi_metadata.h', 'video/drv_hdmi.h', 'uapi/drm/sunxi_drm.h']
    headers += [str(p.relative_to(TINA / 'bsp/include'))
                for p in sorted((TINA / 'bsp/include/errcode').glob('*.h'))]
    for name in headers:
        p = TINA / 'bsp/include' / name
        if not p.exists():
            continue
        target = DEST / 'include' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = p.read_bytes()
        target.write_bytes(data)
        records[str(p.relative_to(TINA))] = hashlib.sha256(data).hexdigest()
    p = DEST / 'Kconfig'
    text = p.read_text().replace('depends on AW_BSP', 'depends on ARCH_SUNXI')
    text = text.replace('depends on DRM && AW_BSP', 'depends on DRM && ARCH_SUNXI')
    text = text.replace('\tselect DRM_KMS_CMA_HELPER\n', '').replace('\tselect DRM_KMS_DMA_HELPER\n', '')
    text = text.replace('drivers/drm/', 'drivers/gpu/drm/t736-bsp/')
    p.write_text(text)
    p = DEST / 'Makefile'
    p.write_text(p.read_text() + '\nccflags-y += -I$(src)/include -DCONFIG_ARCH_SUN60IW2=1\n')
    patch = ROOT / 'patches/t736-display-linux618-compat.patch'
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=KERNEL, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=KERNEL, check=True)
    for filename, line in [
        ('drivers/gpu/drm/Kconfig', '\nsource "drivers/gpu/drm/t736-bsp/Kconfig"\n'),
        ('drivers/gpu/drm/Makefile', '\nobj-$(CONFIG_AW_DRM) += t736-bsp/\n'),
    ]:
        p = KERNEL / filename
        text = p.read_text()
        assert line.strip() not in text
        p.write_text(text + line)
    out = ROOT / 'build/linux-hdmi-v12'
    out.mkdir(exist_ok=True)
    assert not (out / '.config').exists()
    shutil.copyfile(ROOT / 'build/linux-armbian/.config', out / '.config')
    record = {'kind': 't736-display-source-compatibility-import', 'source_root': str(source),
              'destination': str(DEST), 'source_files': records,
              'prebuilt_executables_imported': False, 'tina_modified': False,
              'hdcp_firmware_included': False, 'hardware_tested': False,
              'note': 'HDCP must remain disabled in board DTS; only display source is staged.'}
    (ROOT / 'evidence/display-source-import-20261114.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'source_files': len(records), 'destination': str(DEST)}))


if __name__ == '__main__':
    main()
