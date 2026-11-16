#!/bin/bash
# Build only the isolated IMG BXM DDK; Tina donor is never changed.
set -euo pipefail
root=${T736_ROOT:-/home/ubuntu/t736-mainline}
kernel=${1:-$root/build/linux-gpu-v15}
out=${2:-$root/build/pvr-bxm-v15}
if (($# >= 2)); then shift 2; elif (($# == 1)); then shift; fi
src=$root/sources/work/pvr-bxm-24.2
test -f "$kernel/include/config/kernel.release"
test -f "$kernel/Module.symvers"
test -f "$src/include/pvrversion.h"
mkdir -p "$out"
# Linux 6.18 changes cwd to the external module directory. Materialize the
# donor's source links before Kbuild resolves its relative composite objects.
python3 - "$src" "$out/target_aarch64/kbuild" <<'PY'
from pathlib import Path
import sys
src, dst = map(Path, sys.argv[1:])
for p in src.rglob('*.c'):
    rel = p.relative_to(src)
    if rel.parts[0] == 'build':
        continue
    q = dst / rel
    q.parent.mkdir(parents=True, exist_ok=True)
    if not q.exists():
        q.symlink_to(p)
PY
make -C "$src/build/linux/sunxi_linux" -j"${JOBS:-8}" \
    KERNELDIR="$kernel" OUT="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    KERNEL_CC=aarch64-linux-gnu-gcc KERNEL_LD=aarch64-linux-gnu-ld \
    KERNEL_NM=aarch64-linux-gnu-gcc-nm KERNEL_OBJCOPY=aarch64-linux-gnu-objcopy \
    GPU_BUILD_TYPE=release BUILD=release SUPPORT_LINUX_DVFS=0 SUPPORT_PDVFS=0 \
    SUPPORT_DMA_FENCE=1 SUPPORT_BUFFER_SYNC=1 \
    "$@"
