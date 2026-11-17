#!/bin/sh
# v25: default-off aggregate wall timing only; frozen v24 remains unchanged.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
base_src="$root/sources/work/linux-storage-v24"
base_out="$root/build/linux-storage-v24"
src="$root/sources/work/linux-storage-v25"
out="$root/build/linux-storage-v25"
[ "$(cat "$base_out/include/config/kernel.release")" = 6.18.63-t736-storage-v24 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1])
for rel,h in json.loads((r/'evidence/storage-v24-build-20261116.json').read_text())['files'].items():
    assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
checks={'boards/t736_mmc_hs400_timing_draft.c':'e5874fee67a7a2b04f495b29c7711c9da446b06ab8954c0fa75d23ac97ac2cee',
'boards/t736_mmc_timing_stats_draft.h':'dd507fc5ea4868c3d6b892e096fd56e7ee57833e820b0b4fffd05e78d7f30604',
'tests/test_mmc_timing_stats.py':'7a8a765362daa668b4ab735a536c132c62869602cd419953ccc518ce6824394d',
'boards/t736_mmc_clock_hs200_draft.h':'33f4c8eae7d5b7c73534e07c4c24652a01d0400fc00f286b9212116ee83cd5a2',
'boards/t736_mmc_tuning_draft.h':'2ba574aec0bf5fe06d4d0a90319891c0c3c7b2871cbdddaa6ae004e91d66eeb0',
'boards/t736_mmc_tuning_error_draft.h':'2cd7bc09f6200e47986f0a08e3755033964898b2e9ba10e1add250b2d95ac243',
'boards/t736_mmc_hs400_policy_draft.h':'ae7f25d6309e1f6b1489277b2edab2fd06bc3f7c64f5a05b7103ea8036922384',
'boards/t736_mmc_hs400_tuning_draft.h':'c202f8f5dc5fb625e568c5b8f9f87f2d3c3e2531e2b3a859d361a805fda1c2bf',
'tests/test_mmc_hs400_policy.py':'7d766d78f7a8179adf0ee89b84b6ec8f34bb054fdd2b05f7657ed1c27a9ce8cb',
'sources/work/linux-storage-v24/drivers/mmc/host/t736_mmc_pio.c':'515d7f75b871ee9da838ef71199706ddedb5425ae6cfac186c3089831d8598e5'}
for rel,h in checks.items():assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
PY
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$base_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base_out" "$out"
fi
cp "$root/boards/t736_mmc_hs400_timing_draft.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_clock_hs200_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_error_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_policy_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_timing_stats_draft.h" "$src/drivers/mmc/host/"
python3 "$root/tests/test_mmc_timing_stats.py"
python3 "$root/tests/test_mmc_hs200_tuning.py"
python3 "$root/tests/test_mmc_hs200_error_recovery.py"
python3 "$root/tests/test_mmc_hs400_policy.py"
cp "$base_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-storage-v25'
export KBUILD_BUILD_VERSION=25
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
assert old-new=={'CONFIG_LOCALVERSION="-t736-storage-v24"'}
assert new-old=={'CONFIG_LOCALVERSION="-t736-storage-v25"'}
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-2}" \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-2}" \
    KDEB_PKGVERSION=6.18.63-25 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v25'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for rel,h in json.loads((r/'evidence/storage-v24-build-20261116.json').read_text())['files'].items():
    assert sha(r/rel)==h,rel
assert sha(r/'sources/work/linux-storage-v24/drivers/mmc/host/t736_mmc_pio.c')=='515d7f75b871ee9da838ef71199706ddedb5425ae6cfac186c3089831d8598e5'
dt=out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
assert sha(dt)=='07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244'
assert sha(r/'sources/work/linux-storage-v25/drivers/mmc/host/t736_mmc_pio.c')==sha(r/'boards/t736_mmc_hs400_timing_draft.c')
pkg=r/'build/linux-image-6.18.63-t736-storage-v25_6.18.63-25_arm64.deb'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',dt,
       out/'System.map',out/'Module.symvers',r/'boards/t736_mmc_hs400_timing_draft.c',
       r/'boards/t736_mmc_clock_hs200_draft.h',r/'boards/t736_mmc_tuning_draft.h',
       r/'boards/t736_mmc_tuning_error_draft.h',r/'tests/test_mmc_hs200_error_recovery.py',
       r/'boards/t736_mmc_hs400_policy_draft.h',r/'boards/t736_mmc_hs400_tuning_draft.h',r/'tests/test_mmc_hs400_policy.py',
       r/'boards/t736_mmc_dma_desc.h',r/'tools/build_timing_storage_kernel.sh',
       r/'boards/t736_mmc_timing_stats_draft.h',r/'tests/test_mmc_timing_stats.py',
       r/'evidence/storage-v25-timing-design-20261116.json']
record={'schema':1,'kind':'t736-storage-v25-aggregate-diagnostic-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v25',
 'baseline_release':'6.18.63-t736-storage-v24','configuration_changes':['CONFIG_LOCALVERSION'],
 'source_tree':'sources/work/linux-storage-v25','output_tree':'build/linux-storage-v25',
 'design':'evidence/storage-v25-timing-design-20261116.json',
 'default_collect_timing':False,'default_hs200_enabled':False,'default_hs400_enabled':False,
 'boot_parameters':['t736_mmc_pio.enable_hs200=1','t736_mmc_pio.hs200_max_hz=200000000',
                    't736_mmc_pio.enable_hs400=1','t736_mmc_pio.hs400_max_hz=200000000',
                    't736_mmc_pio.collect_timing=1'],
 'debugfs':'/sys/kernel/debug/t736_mmc_timing/stats',
 'behavior_changes':'None; bounded aggregate observations only; no per-I/O logs',
 'preserved':'v24 tuning/DMA/errors/quarantine/polling/64KiB limit/affinity; same DTB/config except LOCALVERSION',
 'shared_pll_or_rail_writes':False,'dt_delta':'None; exact v24 DTB hash',
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},'frozen_v24_files_unchanged':True,
 'board_validation':'pending; diagnostic-on candidate not throughput optimization or persistent promotion'}
(r/'evidence/storage-v25-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_timing_storage_package.py" --root "$root"
