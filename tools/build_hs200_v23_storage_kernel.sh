#!/bin/sh
# v23: CMD21 card-error summary recovery; frozen v22 remains unchanged.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
base_src="$root/sources/work/linux-storage-v22"
base_out="$root/build/linux-storage-v22"
src="$root/sources/work/linux-storage-v23"
out="$root/build/linux-storage-v23"
[ "$(cat "$base_out/include/config/kernel.release")" = 6.18.63-t736-storage-v22 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1])
for rel,h in json.loads((r/'evidence/storage-v22-build-20261116.json').read_text())['files'].items():
    assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
checks={'boards/t736_mmc_hs200_v23_draft.c':'e812522e9295ab162ae604ff2a0f626d1210ddb1c86a6402102897f30ea39d46',
'boards/t736_mmc_clock_hs200_draft.h':'33f4c8eae7d5b7c73534e07c4c24652a01d0400fc00f286b9212116ee83cd5a2',
'boards/t736_mmc_tuning_draft.h':'2ba574aec0bf5fe06d4d0a90319891c0c3c7b2871cbdddaa6ae004e91d66eeb0',
'boards/t736_mmc_tuning_error_draft.h':'2cd7bc09f6200e47986f0a08e3755033964898b2e9ba10e1add250b2d95ac243',
'sources/work/linux-storage-v22/drivers/mmc/host/t736_mmc_pio.c':'db179f33c161bd134d13698df41f41e6973f5465bd2e2c84777cadca68f57bbe'}
for rel,h in checks.items():assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
PY
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$base_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base_out" "$out"
fi
cp "$root/boards/t736_mmc_hs200_v23_draft.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_clock_hs200_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_error_draft.h" "$src/drivers/mmc/host/"
python3 "$root/tests/test_mmc_hs200_tuning.py"
python3 "$root/tests/test_mmc_hs200_error_recovery.py"
cp "$base_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-storage-v23'
export KBUILD_BUILD_VERSION=23
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
assert old-new=={'CONFIG_LOCALVERSION="-t736-storage-v22"'}
assert new-old=={'CONFIG_LOCALVERSION="-t736-storage-v23"'}
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-23 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v23'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for rel,h in json.loads((r/'evidence/storage-v22-build-20261116.json').read_text())['files'].items():
    assert sha(r/rel)==h,rel
assert sha(r/'sources/work/linux-storage-v22/drivers/mmc/host/t736_mmc_pio.c')=='db179f33c161bd134d13698df41f41e6973f5465bd2e2c84777cadca68f57bbe'
dt=out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
assert sha(dt)=='07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244'
assert sha(r/'sources/work/linux-storage-v23/drivers/mmc/host/t736_mmc_pio.c')==sha(r/'boards/t736_mmc_hs200_v23_draft.c')
pkg=r/'build/linux-image-6.18.63-t736-storage-v23_6.18.63-23_arm64.deb'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',dt,
       out/'System.map',out/'Module.symvers',r/'boards/t736_mmc_hs200_v23_draft.c',
       r/'boards/t736_mmc_clock_hs200_draft.h',r/'boards/t736_mmc_tuning_draft.h',
       r/'boards/t736_mmc_tuning_error_draft.h',r/'tests/test_mmc_hs200_error_recovery.py',
       r/'boards/t736_mmc_dma_desc.h',r/'tools/build_hs200_v23_storage_kernel.sh']
record={'schema':1,'kind':'t736-storage-v23-hs200-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v23',
 'baseline_release':'6.18.63-t736-storage-v22','configuration_changes':['CONFIG_LOCALVERSION'],
 'source_tree':'sources/work/linux-storage-v23','output_tree':'build/linux-storage-v23',
 'design':'evidence/storage-v23-hs200-error-recovery-design-20261116.json',
 'default_hs200_enabled':False,'first_stage_hs200_hz':100000000,'later_stage_hs200_hz':200000000,
 'boot_parameters':['t736_mmc_pio.enable_hs200=1','t736_mmc_pio.hs200_max_hz=100000000'],
 'tuning':'64 linear taps,20 fresh128B patterns/tap,window>=12,16 center verification patterns',
 'flow_control':'HS200 per-request128/512B THLD; preserves SFC/BCIG; restores legacy policy on exit',
 'shared_pll_or_rail_writes':False,'hs400_enabled':False,'dt_delta':'None; exact v22 DTB hash',
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},'frozen_v22_files_unchanged':True,
 'board_validation':'pending100MHz then200MHz one-shot tuning/integrity/performance/GPU; defaults remain existing fallback'}
(r/'evidence/storage-v23-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_hs200_v23_storage_package.py" --root "$root"
