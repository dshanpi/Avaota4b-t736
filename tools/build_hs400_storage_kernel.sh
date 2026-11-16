#!/bin/sh
# v24: opt-in HS400 command/data-strobe tuning; frozen v23 remains unchanged.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
base_src="$root/sources/work/linux-storage-v23"
base_out="$root/build/linux-storage-v23"
src="$root/sources/work/linux-storage-v24"
out="$root/build/linux-storage-v24"
[ "$(cat "$base_out/include/config/kernel.release")" = 6.18.63-t736-storage-v23 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1])
for rel,h in json.loads((r/'evidence/storage-v23-build-20261116.json').read_text())['files'].items():
    assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
checks={'boards/t736_mmc_hs400_draft.c':'515d7f75b871ee9da838ef71199706ddedb5425ae6cfac186c3089831d8598e5',
'boards/t736_mmc_clock_hs200_draft.h':'33f4c8eae7d5b7c73534e07c4c24652a01d0400fc00f286b9212116ee83cd5a2',
'boards/t736_mmc_tuning_draft.h':'2ba574aec0bf5fe06d4d0a90319891c0c3c7b2871cbdddaa6ae004e91d66eeb0',
'boards/t736_mmc_tuning_error_draft.h':'2cd7bc09f6200e47986f0a08e3755033964898b2e9ba10e1add250b2d95ac243',
'boards/t736_mmc_hs400_policy_draft.h':'ae7f25d6309e1f6b1489277b2edab2fd06bc3f7c64f5a05b7103ea8036922384',
'boards/t736_mmc_hs400_tuning_draft.h':'c202f8f5dc5fb625e568c5b8f9f87f2d3c3e2531e2b3a859d361a805fda1c2bf',
'tests/test_mmc_hs400_policy.py':'7d766d78f7a8179adf0ee89b84b6ec8f34bb054fdd2b05f7657ed1c27a9ce8cb',
'sources/work/linux-storage-v23/drivers/mmc/host/t736_mmc_pio.c':'e812522e9295ab162ae604ff2a0f626d1210ddb1c86a6402102897f30ea39d46'}
for rel,h in checks.items():assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
PY
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$base_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base_out" "$out"
fi
cp "$root/boards/t736_mmc_hs400_draft.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_clock_hs200_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_error_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_policy_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_tuning_draft.h" "$src/drivers/mmc/host/"
python3 "$root/tests/test_mmc_hs200_tuning.py"
python3 "$root/tests/test_mmc_hs200_error_recovery.py"
python3 "$root/tests/test_mmc_hs400_policy.py"
cp "$base_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-storage-v24'
export KBUILD_BUILD_VERSION=24
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
assert old-new=={'CONFIG_LOCALVERSION="-t736-storage-v23"'}
assert new-old=={'CONFIG_LOCALVERSION="-t736-storage-v24"'}
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-24 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v24'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for rel,h in json.loads((r/'evidence/storage-v23-build-20261116.json').read_text())['files'].items():
    assert sha(r/rel)==h,rel
assert sha(r/'sources/work/linux-storage-v23/drivers/mmc/host/t736_mmc_pio.c')=='e812522e9295ab162ae604ff2a0f626d1210ddb1c86a6402102897f30ea39d46'
dt=out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
assert sha(dt)=='07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244'
assert sha(r/'sources/work/linux-storage-v24/drivers/mmc/host/t736_mmc_pio.c')==sha(r/'boards/t736_mmc_hs400_draft.c')
pkg=r/'build/linux-image-6.18.63-t736-storage-v24_6.18.63-24_arm64.deb'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',dt,
       out/'System.map',out/'Module.symvers',r/'boards/t736_mmc_hs400_draft.c',
       r/'boards/t736_mmc_clock_hs200_draft.h',r/'boards/t736_mmc_tuning_draft.h',
       r/'boards/t736_mmc_tuning_error_draft.h',r/'tests/test_mmc_hs200_error_recovery.py',
       r/'boards/t736_mmc_hs400_policy_draft.h',r/'boards/t736_mmc_hs400_tuning_draft.h',r/'tests/test_mmc_hs400_policy.py',
       r/'boards/t736_mmc_dma_desc.h',r/'tools/build_hs400_storage_kernel.sh']
record={'schema':1,'kind':'t736-storage-v24-hs400-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v24',
 'baseline_release':'6.18.63-t736-storage-v23','configuration_changes':['CONFIG_LOCALVERSION'],
 'source_tree':'sources/work/linux-storage-v24','output_tree':'build/linux-storage-v24',
 'design':'evidence/storage-hs400-design-20261116.json',
 'default_hs200_enabled':False,'default_hs400_enabled':False,'hs200_prerequisite_hz':200000000,
 'first_stage_hs400_hz':100000000,'later_stage_hs400_hz':200000000,
 'boot_parameters':['t736_mmc_pio.enable_hs200=1','t736_mmc_pio.hs200_max_hz=200000000',
                    't736_mmc_pio.enable_hs400=1','t736_mmc_pio.hs400_max_hz=100000000'],
 'tuning':'HS200 CMD21 prerequisite; HS400 independent64linear CMD13/512B EXT_CSD eyes,20trials/tap,min12,eachcenter16; no retries/golden reads',
 'flow_control':'HS200/HS400 per-request128/512B THLD; preserves SFC/BCIG; restores legacy policy on exit',
 'shared_pll_or_rail_writes':False,'hs400_capability_opt_in':True,'dt_delta':'None; exact v23 DTB hash',
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},'frozen_v23_files_unchanged':True,
 'board_validation':'pendingHS400100MHz then200MHz actualmode+CMD/DS eyes+integrity/performance/GPU; defaults remain existing fallback'}
(r/'evidence/storage-v24-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_hs400_storage_package.py" --root "$root"
