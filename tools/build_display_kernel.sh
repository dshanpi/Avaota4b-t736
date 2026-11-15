#!/bin/sh
# Isolated display candidate; does not install anything on the board.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux"
revision=${1:-12}
case "$revision" in 12|13) ;; *) exit 2 ;; esac
out="$root/build/linux-hdmi-v$revision"
export KBUILD_BUILD_VERSION=$revision
mkdir -p "$out"
if [ ! -f "$out/.config" ]; then
    cp "$root/userpatches/linux-t736-hdmi-v12.config" "$out/.config"
fi
cp "$root/boards/clk-t736-display.c" "$src/drivers/clk/"
cp "$root/boards/t736-display-power.c" "$src/drivers/pmdomain/sunxi/"
cp "$root/boards/t736-hdmi.dtsi" "$src/arch/arm64/boot/dts/allwinner/"
python3 - "$src" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1])
for file, line in [
    ('drivers/clk/Makefile', 'obj-$(CONFIG_AW_DRM) += clk-t736-display.o'),
    ('drivers/pmdomain/sunxi/Makefile', 'obj-$(CONFIG_AW_DRM) += t736-display-power.o'),
    ('arch/arm64/boot/dts/allwinner/Makefile', 'dtb-y += t736-hdmi-candidate.dtb'),
]:
    p = src / file
    text = p.read_text()
    if line not in text:
        p.write_text(text + '\n' + line + '\n')
dts = src / 'arch/arm64/boot/dts/allwinner'
(dts / 't736-hdmi-candidate.dts').write_text(
    (dts / 't736-mainline-bringup.dts').read_text() + '\n#include "t736-hdmi.dtsi"\n')
PY
cfg="$src/scripts/config"
for symbol in PM PM_GENERIC_DOMAINS PM_GENERIC_DOMAINS_OF CMA DMA_CMA \
    DRM AW_DRM AW_DRM_DE AW_DRM_TCON AW_DRM_TCON_TV AW_DRM_TCON_TOP \
    AW_DRM_HDMI_TX AW_DRM_HDMI20 FB FRAMEBUFFER_CONSOLE; do
    "$cfg" --file "$out/.config" --enable "$symbol"
done
"$cfg" --file "$out/.config" --set-val CMA_SIZE_MBYTES 128 \
    --set-str LOCALVERSION "-t736-hdmi-v$revision" --disable LOCALVERSION_AUTO \
    --set-str CMDLINE 'earlycon=uart8250,mmio32,0x02500000 console=tty0 console=ttyS0,115200 clk_ignore_unused'
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
for symbol in PM PM_GENERIC_DOMAINS PM_GENERIC_DOMAINS_OF CMA DMA_CMA \
    DRM AW_DRM AW_DRM_DE AW_DRM_TCON AW_DRM_TCON_TV AW_DRM_TCON_TOP \
    AW_DRM_HDMI_TX AW_DRM_HDMI20 FRAMEBUFFER_CONSOLE; do
    grep -qx "CONFIG_$symbol=y" "$out/.config"
done
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-hdmi-candidate.dtb modules
nm "$out/vmlinux" | grep -q ' t display_power_probe$'
nm "$out/vmlinux" | grep -q ' t t736_display_ccu_probe$'
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-$revision bindeb-pkg
sha256sum "$out/arch/arm64/boot/Image" \
    "$out/arch/arm64/boot/dts/allwinner/t736-hdmi-candidate.dtb" \
    "$root/build/linux-image-6.18.63-t736-hdmi-v${revision}_6.18.63-${revision}_arm64.deb"
