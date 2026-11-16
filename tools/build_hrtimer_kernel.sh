#!/bin/sh
# v20: enable high-resolution timers; reuse frozen v19 source READ-ONLY.
# Copy only its object tree, keeping compiler source paths stable for incremental builds.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux-storage-v19"
base="$root/build/linux-storage-v19"
out="$root/build/linux-storage-v20"
[ "$(cat "$base/include/config/kernel.release")" = 6.18.63-t736-storage-v19 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib, sys
r=Path(sys.argv[1])
checks={
 'sources/work/linux-storage-v19/drivers/mmc/host/t736_mmc_pio.c':'f29554e12658287abbd4af5068d1c8fa567946af7b281e6d032441f8a295e639',
 'sources/work/linux-storage-v19/drivers/mmc/host/t736_mmc_dma_desc.h':'d1936275bae1cf1ddc83044523ce58a003b8286bfd3e01b1c7762cf3cc6e1f0c',
 'sources/work/linux-storage-v19/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts':'2d4f935044ae65104f75f3773c4416939c2401e8cbfc90b7d4c3de48545bddd2',
 'build/linux-storage-v19/arch/arm64/boot/Image':'34522f80371d20ca9e76cfebcb1c025061d569997defe9b23aa47214bb89c12b',
 'build/linux-storage-v19/.config':'6aab4229067aee5bea1c432d441cd4083b532766d400507e45bb4396c0672778',
}
for name, expected in checks.items():
    if hashlib.sha256((r/name).read_bytes()).hexdigest()!=expected:
        raise SystemExit('Frozen v19 input changed: '+name)
PY
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base" "$out"
fi
cp "$base/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --enable HIGH_RES_TIMERS \
    --set-str LOCALVERSION '-t736-storage-v20'
export KBUILD_BUILD_VERSION=20
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
removed=old-new
added=new-old
expected_removed={'CONFIG_LOCALVERSION="-t736-storage-v19"','# CONFIG_HIGH_RES_TIMERS is not set'}
expected_added={'CONFIG_LOCALVERSION="-t736-storage-v20"','CONFIG_HIGH_RES_TIMERS=y',
                'CONFIG_TICK_ONESHOT=y','CONFIG_SCHED_HRTICK=y'}
if removed!=expected_removed or added!=expected_added:
    raise SystemExit(f'Unexpected configuration delta: removed={removed}, added={added}')
print('PASS: HRT plus derived TICK_ONESHOT/SCHED_HRTICK and release only; HZ250/PERIODIC unchanged')
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
[ "$(cat "$out/include/config/kernel.release")" = 6.18.63-t736-storage-v20 ]
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-20 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v20'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pkg=r/'build/linux-image-6.18.63-t736-storage-v20_6.18.63-20_arm64.deb'
baseline=json.loads((r/'evidence/storage-v19-build-20261116.json').read_text())
for name, expected in baseline['files'].items():
    if sha(r/name)!=expected:
        raise SystemExit('Frozen v19 artifact/source changed: '+name)
dt=out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
assert sha(dt)=='b9ca0c0716e7759988aff3ce87a20200d1d8ac98de559569d34d8aaf83402b9c'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',
       dt,out/'System.map',out/'Module.symvers',r/'tools/build_hrtimer_kernel.sh']
record={'schema':1,'kind':'t736-storage-v20-high-resolution-timer-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v20',
 'baseline_release':'6.18.63-t736-storage-v19',
 'source_tree':'sources/work/linux-storage-v19 (shared frozen source, no edits)',
 'output_tree':'build/linux-storage-v20 (independent copy, stable source paths)',
 'configuration_changes':['CONFIG_HIGH_RES_TIMERS=y','CONFIG_TICK_ONESHOT=y (selected dependency)',
                          'CONFIG_SCHED_HRTICK=y (def_bool HIGH_RES_TIMERS)','CONFIG_LOCALVERSION'],
 'scheduler_hrticks':'CONFIG_SCHED_HRTICK is implicit; runtime HRTICK/HRTICK_DL features remain default false',
 'unchanged':['HZ250','HZ_PERIODIC','12MHz legacy8bit','IDIE/DMA implementation','CPU topology/DTB','voltage/PLL/gates','media drivers'],
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},
 'frozen_v19_verified_unchanged':baseline['files'],
 'board_validation':'pending: check high-resolution activation, bounded nanosleep/poll latency, boot time, scratch-file hashes/read-write throughput and GPU/desktop; retain v16 default rollback'}
(r/'evidence/storage-v20-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_hrtimer_package.py" --root "$root"
