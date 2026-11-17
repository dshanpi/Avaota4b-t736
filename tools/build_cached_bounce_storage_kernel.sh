#!/bin/sh
# v26: cacheable noncoherent payload with explicit ownership; frozen v25 unchanged.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
base_src="$root/sources/work/linux-storage-v25"
base_out="$root/build/linux-storage-v25"
src="$root/sources/work/linux-storage-v26"
out="$root/build/linux-storage-v26"
[ "$(cat "$base_out/include/config/kernel.release")" = 6.18.63-t736-storage-v25 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1])
for rel,h in json.loads((r/'evidence/storage-v25-build-20261116.json').read_text())['files'].items():
    assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
checks={'boards/t736_mmc_cached_bounce_draft.c':'2b1f48ba68789523b832d9226ad1e0031f17376d3f0abea816fd97de8f1f9ab6',
'tests/test_mmc_cached_bounce.py':'062ff9e38a7b46c7e9e5f778ba82ccf7f8da6a9ba2dc6aaac66beff445da4dc7',
'boards/t736_mmc_timing_stats_draft.h':'dd507fc5ea4868c3d6b892e096fd56e7ee57833e820b0b4fffd05e78d7f30604',
'tests/test_mmc_timing_stats.py':'7a8a765362daa668b4ab735a536c132c62869602cd419953ccc518ce6824394d',
'boards/t736_mmc_clock_hs200_draft.h':'33f4c8eae7d5b7c73534e07c4c24652a01d0400fc00f286b9212116ee83cd5a2',
'boards/t736_mmc_tuning_draft.h':'2ba574aec0bf5fe06d4d0a90319891c0c3c7b2871cbdddaa6ae004e91d66eeb0',
'boards/t736_mmc_tuning_error_draft.h':'2cd7bc09f6200e47986f0a08e3755033964898b2e9ba10e1add250b2d95ac243',
'boards/t736_mmc_hs400_policy_draft.h':'ae7f25d6309e1f6b1489277b2edab2fd06bc3f7c64f5a05b7103ea8036922384',
'boards/t736_mmc_hs400_tuning_draft.h':'c202f8f5dc5fb625e568c5b8f9f87f2d3c3e2531e2b3a859d361a805fda1c2bf',
'tests/test_mmc_hs400_policy.py':'7d766d78f7a8179adf0ee89b84b6ec8f34bb054fdd2b05f7657ed1c27a9ce8cb',
'sources/work/linux-storage-v25/drivers/mmc/host/t736_mmc_pio.c':'e5874fee67a7a2b04f495b29c7711c9da446b06ab8954c0fa75d23ac97ac2cee'}
for rel,h in checks.items():assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
config=(r/'build/linux-storage-v25/.config').read_text()
for key in ['CONFIG_HAS_DMA=y','CONFIG_DMA_NEED_SYNC=y',
            'CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU=y','CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE=y']:
    assert key+'\n' in config,key
for rel,h in json.loads((r/'evidence/storage-v26-cached-bounce-design-20261116.json').read_text())['kernel_api_sources'].items():
    used=rel.replace('sources/linux/','sources/work/linux-storage-v25/')
    assert hashlib.sha256((r/used).read_bytes()).hexdigest()==h,used
PY
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$base_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base_out" "$out"
fi
cp "$root/boards/t736_mmc_cached_bounce_draft.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_clock_hs200_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_tuning_error_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_policy_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_hs400_tuning_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_timing_stats_draft.h" "$src/drivers/mmc/host/"
python3 "$root/tests/test_mmc_cached_bounce.py"
python3 "$root/tests/test_mmc_timing_stats.py"
python3 "$root/tests/test_mmc_hs200_tuning.py"
python3 "$root/tests/test_mmc_hs200_error_recovery.py"
python3 "$root/tests/test_mmc_hs400_policy.py"
cp "$base_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-storage-v26'
export KBUILD_BUILD_VERSION=26
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
assert old-new=={'CONFIG_LOCALVERSION="-t736-storage-v25"'}
assert new-old=={'CONFIG_LOCALVERSION="-t736-storage-v26"'}
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-8}" \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-8}" \
    KDEB_PKGVERSION=6.18.63-26 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v26'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for rel,h in json.loads((r/'evidence/storage-v25-build-20261116.json').read_text())['files'].items():
    assert sha(r/rel)==h,rel
assert sha(r/'sources/work/linux-storage-v25/drivers/mmc/host/t736_mmc_pio.c')=='e5874fee67a7a2b04f495b29c7711c9da446b06ab8954c0fa75d23ac97ac2cee'
dt=out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
assert sha(dt)=='07824de9f58de27e0aebfaf70e4da464d145570e0235f17bcbd01abed39ff244'
assert sha(r/'sources/work/linux-storage-v26/drivers/mmc/host/t736_mmc_pio.c')==sha(r/'boards/t736_mmc_cached_bounce_draft.c')
pkg=r/'build/linux-image-6.18.63-t736-storage-v26_6.18.63-26_arm64.deb'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',dt,
       out/'System.map',out/'Module.symvers',r/'boards/t736_mmc_cached_bounce_draft.c',
       r/'boards/t736_mmc_clock_hs200_draft.h',r/'boards/t736_mmc_tuning_draft.h',
       r/'boards/t736_mmc_tuning_error_draft.h',r/'tests/test_mmc_hs200_error_recovery.py',
       r/'boards/t736_mmc_hs400_policy_draft.h',r/'boards/t736_mmc_hs400_tuning_draft.h',r/'tests/test_mmc_hs400_policy.py',
       r/'boards/t736_mmc_dma_desc.h',r/'tools/build_cached_bounce_storage_kernel.sh',
       r/'boards/t736_mmc_timing_stats_draft.h',r/'tests/test_mmc_timing_stats.py',
       r/'tests/test_mmc_cached_bounce.py',
       r/'evidence/storage-v26-cached-bounce-design-20261116.json']
record={'schema':1,'kind':'t736-storage-v26-cacheable-bounce-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v26',
 'baseline_release':'6.18.63-t736-storage-v25','configuration_changes':['CONFIG_LOCALVERSION'],
 'source_tree':'sources/work/linux-storage-v26','output_tree':'build/linux-storage-v26',
 'design':'evidence/storage-v26-cached-bounce-design-20261116.json',
 'default_collect_timing':False,'default_hs200_enabled':False,'default_hs400_enabled':False,
 'boot_parameters':['t736_mmc_pio.enable_hs200=1','t736_mmc_pio.hs200_max_hz=200000000',
                    't736_mmc_pio.enable_hs400=1','t736_mmc_pio.hs400_max_hz=200000000',
                    't736_mmc_pio.collect_timing=1'],
 'debugfs':'/sys/kernel/debug/t736_mmc_timing/stats',
 'behavior_changes':'Payload dma_alloc_noncoherent with full64KiB BIDIRECTIONAL sync ownership; coherent descriptors unchanged',
 'preserved':'v25 tuning/descriptor DMA/errors/quarantine/polling/64KiB limit/affinity/diagnostic call sites; same DTB/config except LOCALVERSION',
 'shared_pll_or_rail_writes':False,'dt_delta':'None; exact v25 DTB hash',
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},'frozen_v25_files_unchanged':True,
 'board_validation':'pending; A/B against v25 with identical HS400200/diagnostic flags, integrity and fresh tuning checks required; no persistent promotion'}
(r/'evidence/storage-v26-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_cached_bounce_storage_package.py" --root "$root"
