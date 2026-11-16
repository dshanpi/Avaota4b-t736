#!/bin/sh
# v21: isolated HS SDR25MHz default, using already-enabled PERI0_800M.
# No shared PLL/rail changes; v20 HRT/DMA and v16 fallback remain frozen.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
base_src="$root/sources/work/linux-storage-v19"
base_out="$root/build/linux-storage-v20"
src="$root/sources/work/linux-storage-v21"
out="$root/build/linux-storage-v21"
[ "$(cat "$base_out/include/config/kernel.release")" = 6.18.63-t736-storage-v20 ]
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1])
for rel,h in json.loads((r/'evidence/storage-v20-build-20261116.json').read_text())['files'].items():
    assert hashlib.sha256((r/rel).read_bytes()).hexdigest()==h,rel
assert hashlib.sha256((r/'sources/work/linux-storage-v19/drivers/mmc/host/t736_mmc_pio.c').read_bytes()).hexdigest()=='f29554e12658287abbd4af5068d1c8fa567946af7b281e6d032441f8a295e639'
PY
if [ ! -d "$src" ]; then
    cp -a --reflink=auto "$base_src" "$src"
fi
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$base_out" "$out"
fi
cp "$root/boards/t736_mmc_hs_draft.c" "$src/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/t736_mmc_clock_hs_draft.h" "$src/drivers/mmc/host/"
cp "$root/boards/t736_mmc_dma_desc.h" "$src/drivers/mmc/host/"
python3 "$root/tests/test_mmc_hs_clock.py"
python3 - "$base_src" "$src" "$root" <<'PY'
from pathlib import Path
import sys
base,src,root=map(Path,sys.argv[1:])
rel='arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts'
(src/rel).write_text((base/rel).read_text()+'\n'+(root/'boards/t736-smhc3-hs-draft.dtsi').read_text())
PY
cp "$base_out/.config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-storage-v21'
export KBUILD_BUILD_VERSION=21
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
python3 - "$base_out/.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
old,new=[set(Path(p).read_text().splitlines()) for p in sys.argv[1:]]
assert old-new=={'CONFIG_LOCALVERSION="-t736-storage-v20"'}
assert new-old=={'CONFIG_LOCALVERSION="-t736-storage-v21"'}
PY
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
python3 - "$root" <<'PY'
from pathlib import Path
import struct,sys
r=Path(sys.argv[1]);sys.path.insert(0,str(r/'tools'))
from repack_storage_v18_topology import fdt
old,old_mem=fdt(r/'build/linux-storage-v20/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb')
new,new_mem=fdt(r/'build/linux-storage-v21/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb')
assert old_mem==new_mem and old.keys()==new.keys()
for node,props in old.items():
    expected=dict(props)
    if node=='/mmc@4023000':
        expected['reg']+=struct.pack('>8I',0,0x02002000,0,4,0,0x020020a0,0,16)
        expected['clocks']=old['/dcxo']['phandle']
        expected['clock-names']=b'dcxo\0'
    assert expected==new[node],node
print('PASS: DT semantic delta limited to read-only REF/PERI0 resources and dcxo handle')
PY
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-21 bindeb-pkg
python3 - "$root" <<'PY'
from pathlib import Path
import hashlib,json,sys
r=Path(sys.argv[1]);out=r/'build/linux-storage-v21'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for rel,h in json.loads((r/'evidence/storage-v20-build-20261116.json').read_text())['files'].items():
    assert sha(r/rel)==h,rel
assert sha(r/'sources/work/linux-storage-v19/drivers/mmc/host/t736_mmc_pio.c')=='f29554e12658287abbd4af5068d1c8fa567946af7b281e6d032441f8a295e639'
pkg=r/'build/linux-image-6.18.63-t736-storage-v21_6.18.63-21_arm64.deb'
files=[out/'.config',out/'arch/arm64/boot/Image',out/'arch/arm64/boot/Image.gz',
       out/'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb',out/'System.map',
       out/'Module.symvers',r/'boards/t736_mmc_hs_draft.c',
       r/'boards/t736_mmc_clock_hs_draft.h',r/'boards/t736_mmc_dma_desc.h',
       r/'boards/t736-smhc3-hs-draft.dtsi',r/'tools/build_hs_storage_kernel.sh',
       r/'sources/work/linux-storage-v21/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts']
record={'schema':1,'kind':'t736-storage-v21-hs-sdr-candidate',
 'status':'compiled-not-board-accepted','kernel_release':'6.18.63-t736-storage-v21',
 'baseline_release':'6.18.63-t736-storage-v20','configuration_changes':['CONFIG_LOCALVERSION'],
 'source_tree':'sources/work/linux-storage-v21','output_tree':'build/linux-storage-v21',
 'storage':'v19 IDIE DMA with v20 HRT; HS SDR25MHz default, opt-in50MHz',
 'parent':'read-only verify existing PERI0_800M with FREF_SEL/SDM/lock/output checks; SMHC3 mux1',
 'shared_pll_or_rail_writes':False,'default_hs_card_hz':25000000,
 'dt_delta':'Only read-only REF/PERI0 register resources and dcxo handle; semantic comparison passed',
 'package':{'path':str(pkg.relative_to(r)),'size':pkg.stat().st_size,'sha256':sha(pkg)},
 'files':{str(p.relative_to(r)):sha(p) for p in files},'frozen_v20_files_unchanged':True,
 'board_validation':'pending25MHz then50MHz one-shot integrity/performance/GPU checks; v16 defaults preserved'}
(r/'evidence/storage-v21-build-20261116.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
python3 "$root/tools/validation/audit_hs_storage_package.py" --root "$root"
