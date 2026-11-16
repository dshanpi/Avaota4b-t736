#!/bin/sh
# Isolated DMA event candidate based on frozen v18. v16 remains board fallback.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
production_src="$root/sources/work/linux-storage-v18"
production_out="$root/build/linux-storage-v18"
src="$root/sources/work/linux-storage-v19"
out="$root/build/linux-storage-v19"
[ "$(cat "$production_out/include/config/kernel.release")" = 6.18.63-t736-storage-v18 ]
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$production_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$production_out" "$out"
fi
cp "$root/boards/t736_mmc_dma_v19.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_dma_desc.h" "$src/drivers/mmc/host/t736_mmc_dma_desc.h"
python3 "$root/tests/test_mmc_dma_descriptor.py"
cp "$production_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" \
    --set-str LOCALVERSION '-t736-storage-v19'
cp "$production_src/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts" "$src/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts"
export KBUILD_BUILD_VERSION=19
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$production_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old, new = [set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
old -= {'CONFIG_LOCALVERSION="-t736-storage-v18"'}
new -= {'CONFIG_LOCALVERSION="-t736-storage-v19"'}
if old != new:
    raise SystemExit(f'Unexpected configuration changes: removed={old-new}, added={new-old}')
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
[ "$(cat "$out/include/config/kernel.release")" = 6.18.63-t736-storage-v19 ]
mkdir -p "$out/module-staging"
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    INSTALL_MOD_PATH="$out/module-staging" modules_install
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-19 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib, json, sys
root = Path(sys.argv[1])
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
out = root/'build/linux-storage-v19'
package = root/'build/linux-image-6.18.63-t736-storage-v19_6.18.63-19_arm64.deb'
record = {
    'schema': 1, 'kind': 't736-dma-v19-kernel-candidate',
    'status': 'compiled-not-board-accepted', 'kernel_release': '6.18.63-t736-storage-v19',
    'baseline_release': '6.18.63-t736-storage-v18',
    'configuration_changes': ['CONFIG_LOCALVERSION'],
    'dt_changes': [],
    'delta_from_v18': ['IDIE enables directional completion plus valid SMHC3 error events; CTRL IRQ remains off', 'Before-reset failure snapshot includes descriptor OWN and full 11-bit FIFO count'],
    'storage_clock_hz': 12000000, 'max_request_bytes': 65536,
    'dma_enabled': True, 'dma_address_bits': 32, 'descriptor_address_shift': 2, 'descriptor_segment_bytes': 1024, 'voltage_or_timing_change': False,
    'package': {'path': str(package.relative_to(root)), 'size': package.stat().st_size,
                'sha256': sha(package)},
    'baseline_files': {str(p.relative_to(root)): sha(p) for p in [
        root/'build/linux-storage-v18/.config',
        root/'build/linux-storage-v18/arch/arm64/boot/Image',
        root/'boards/t736_mmc_dma_v18.c']},
    'files': {str(p.relative_to(root)): sha(p) for p in [
        root/'boards/t736_mmc_dma_v19.c', root/'boards/t736_mmc_dma_desc.h', root/'sources/work/linux-storage-v19/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts', out/'.config',
        out/'arch/arm64/boot/Image',
        out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb',
        out/'Module.symvers']},
    'board_validation': 'pending; verify cold boot, rootfs reads, bounded scratch-file write/read hashes, throughput and dmesg before promotion'
}
target = root/'evidence/storage-v19-build-20261116.json'
target.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
print(json.dumps(record, indent=2))
PY
python3 "$root/tools/validation/audit_dma_event_package.py" --root "$root"
