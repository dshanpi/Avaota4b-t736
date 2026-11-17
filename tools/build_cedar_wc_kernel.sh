#!/bin/sh
# Isolated v27 decoder experiment, based on the proven v26 storage/GPU tree.
set -eu
root=/home/ubuntu/t736-mainline
src=$root/sources/work/linux-media-v27
out=$root/build/linux-media-v27
test -f "$src/Makefile"
test -f "$out/.config"
cmp "$src/drivers/mmc/host/t736_mmc_pio.c" "$root/sources/work/linux-storage-v26/drivers/mmc/host/t736_mmc_pio.c"
cp "$root/boards/clk-t736-ve.c" "$src/drivers/clk/"
cp "$root/boards/t736-ve-power.c" "$src/drivers/soc/sunxi/"
cp "$root/boards/t736-cedar-wc.c" "$src/drivers/misc/"
cp "$root/boards/t736-media-v27.dtsi" "$src/arch/arm64/boot/dts/allwinner/"
python3 - "$src" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1])
for rel,text in {
 'drivers/clk/Makefile':'obj-$(CONFIG_VIDEO_T736_CEDAR_WC) += clk-t736-ve.o',
 'drivers/soc/sunxi/Makefile':'obj-$(CONFIG_VIDEO_T736_CEDAR_WC) += t736-ve-power.o',
 'drivers/misc/Makefile':'obj-$(CONFIG_VIDEO_T736_CEDAR_WC) += t736-cedar-wc.o',
 'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dts':'#include "t736-media-v27.dtsi"',
}.items():
 p=s/rel
 if text not in p.read_text(): p.write_text(p.read_text()+'\n'+text+'\n')
p=s/'drivers/misc/Kconfig'
if 'config VIDEO_T736_CEDAR_WC' not in p.read_text():
 p.write_text(p.read_text()+'''\nconfig VIDEO_T736_CEDAR_WC
 bool "T736 private Cedar WC decoder experiment"
 depends on ARM64 && OF && DMA_CMA && PM_GENERIC_DOMAINS && COMMON_CLK && RESET_CONTROLLER
 select DMABUF_HEAPS
 help
   Root-only fixed-frequency decoder with a bounded private contiguous heap.
   Imported allocations remain pinned until reboot. Not a production driver.
''')
PY
"$src/scripts/config" --file "$out/.config" --set-str LOCALVERSION '-t736-media-v27' \
    --enable VIDEO_T736_CEDAR_WC --enable DMABUF_HEAPS --disable AW_IOMMU \
    --disable DMABUF_HEAPS_SYSTEM --disable DMABUF_HEAPS_CMA
export KBUILD_BUILD_VERSION=27
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
grep -qx 'CONFIG_VIDEO_T736_CEDAR_WC=y' "$out/.config"
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-27 bindeb-pkg
bash "$root/tools/build_gpu_module.sh" "$out" "$root/build/pvr-bxm-media-v27"
sha256sum "$root/build/linux-image-6.18.63-t736-media-v27_6.18.63-27_arm64.deb" \
    "$out/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb" \
    "$root/build/pvr-bxm-media-v27/target_aarch64/kbuild/pvrsrvkm.ko"
