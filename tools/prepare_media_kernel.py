#!/usr/bin/env python3
"""Prepare a compile-only T736 Linux 6.18 media tree; never change a board.

The imported IOMMU donor deliberately has no enabled device-tree node. Build/API
checks are a preparation stage, not permission to deploy this incomplete port.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    ap.add_argument('--tina', type=Path, default=Path('/home/ubuntu/t736-aiot/tina'))
    args = ap.parse_args()
    root, tina = args.root.resolve(), args.tina.resolve()
    baseline = root / 'sources/work/linux-storage-v16'
    baseline_out = root / 'build/linux-storage-v16'
    src, out = root / 'sources/work/linux-media-v17', root / 'build/linux-media-v17'
    if src.exists() or out.exists():
        ap.error('media source/output already exists; refusing to overwrite')
    assert not (baseline / '.git').exists(), 'Use managed worktree tooling for Git worktrees'
    assert (baseline_out / 'include/config/kernel.release').read_text().strip() == '6.18.63-t736-storage-v16'
    subprocess.run(['cp', '-a', '--reflink=auto', str(baseline), str(src)], check=True)
    subprocess.run(['cp', '-a', '--reflink=auto', str(baseline_out), str(out)], check=True)
    port = src / 'drivers/iommu/t736'
    port.mkdir()
    (port / 'include').mkdir()
    inputs = []
    names = ['sunxi-iommu-common.c', 'sunxi-iommu-pgtable.c', 'sunxi-iommu-v2.c',
             'sunxi-iommu.h', 'sunxi-iommu-pgtable.h']
    for name in names:
        donor = tina / 'bsp/drivers/iommu' / name
        target = port / name
        raw = donor.read_bytes()
        # This is only a header location change, not an IOMMU API shim.
        raw = raw.replace(b'#include <../drivers/iommu/dma-iommu.h>',
                          b'#include "../dma-iommu.h"')
        target.write_bytes(raw)
        inputs.append({'source': donor.relative_to(tina).as_posix(), 'source_sha256': sha(donor),
                       'target': target.relative_to(src).as_posix(), 'target_sha256': sha(target)})
    donor = tina / 'bsp/include/sunxi-iommu.h'
    shutil.copy2(donor, port / 'include/sunxi-iommu.h')
    inputs.append({'source': donor.relative_to(tina).as_posix(), 'source_sha256': sha(donor),
                   'target': 'drivers/iommu/t736/include/sunxi-iommu.h', 'target_sha256': sha(donor)})
    (port / 'Makefile').write_text(
        'obj-$(CONFIG_AW_IOMMU) += t736-iommu-v20.o\n'
        't736-iommu-v20-y := sunxi-iommu-common.o sunxi-iommu-pgtable.o sunxi-iommu-v2.o\n'
        'ccflags-y += -I$(src)/include\n')
    (port / 'Kconfig').write_text(
        'config AW_IOMMU\n\tbool "T736 IOMMU v20 compile-only candidate"\n'
        '\tdepends on ARM64 && OF\n\tselect IOMMU_API\n\tselect IOMMU_DMA\n'
        '\tselect AW_IOMMU_V2\n\tselect AW_IOMMU_DISTRIBUTE\n'
        '\thelp\n\t  Incomplete port; no enabled device-tree node is supplied.\n'
        'config AW_IOMMU_V2\n\tbool\n'
        'config AW_IOMMU_DISTRIBUTE\n\tbool\n')
    with (src / 'drivers/iommu/Kconfig').open('a') as stream:
        stream.write('\nsource "drivers/iommu/t736/Kconfig"\n')
    with (src / 'drivers/iommu/Makefile').open('a') as stream:
        stream.write('\nobj-$(CONFIG_AW_IOMMU) += t736/\n')
    subprocess.run([str(src / 'scripts/config'), '--file', str(out / '.config'),
                    '--enable', 'IOMMU_SUPPORT', '--enable', 'AW_IOMMU',
                    '--enable', 'DMABUF_HEAPS', '--enable', 'DMABUF_HEAPS_SYSTEM',
                    '--set-str', 'LOCALVERSION', '-t736-media-v17'], check=True)
    subprocess.run(['make', '-C', str(src), 'O=' + str(out), 'ARCH=arm64',
                    'CROSS_COMPILE=aarch64-linux-gnu-', 'olddefconfig'], check=True)
    record = {'schema': 1, 'status': 'isolated compile-only preparation; not deployable',
              'baseline': str(baseline), 'source': str(src), 'output': str(out),
              'enabled_hardware_nodes': [], 'board_access': False,
              'baseline_config_sha256': sha(baseline_out / '.config'),
              'candidate_config_sha256': sha(out / '.config'),
              'imports': inputs,
              'intentional_pending': ['Linux 6.18 IOMMU API port',
                                     'domain/group lifetime and attach semantics',
                                     'VE/NSI/IOMMU clock and reset providers',
                                     'VE_DEC power domain', 'Cedar ioctl/DMA safety port']}
    path = root / 'evidence/media-v17-prepare-20261116.json'
    path.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
