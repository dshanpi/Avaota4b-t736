#!/bin/sh
# Fixed-frequency IMG BXM candidate. Keep v14 and its build output intact.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux"
sh "$root/tools/apply_display_fixes.sh"
out="$root/build/linux-gpu-v15"
export KBUILD_BUILD_VERSION=15
if [ ! -d "$out" ]; then
    cp -a --reflink=auto "$root/build/linux-hdmi-v14" "$out"
fi
cp "$root/boards/clk-t736-gpu.c" "$src/drivers/clk/"
cp "$root/boards/t736-gpu-power.c" "$src/drivers/pmdomain/sunxi/"
cp "$root/boards/t736-gpu-supply.c" "$src/drivers/regulator/"
cp "$root/boards/t736-pmic.h" "$src/include/linux/"
cp "$root/boards/t736-gpu.dtsi" "$root/boards/t736-gpu-supply.dtsi" "$src/arch/arm64/boot/dts/allwinner/"
python3 - "$root" "$src" <<'PY'
from pathlib import Path
import sys
root, src = map(Path, sys.argv[1:])
# Reuse the hardware-tested SPL's exact byte transaction state machine.
h = (root/'spl/early/twi.h').read_text().replace('#include <stdint.h>', '#include <linux/types.h>')
c = (root/'spl/early/twi.c').read_text().replace('#include "twi.h"', '')
text = (h + '\n' + c).replace('uint32_t', 'u32').replace('uint8_t', 'u8')
(src/'drivers/regulator/t736-pmic-twi.inc').write_text(text)
for file, line in [
    ('drivers/clk/Makefile', 'obj-$(CONFIG_AW_DRM) += clk-t736-gpu.o'),
    ('drivers/pmdomain/sunxi/Makefile', 'obj-$(CONFIG_AW_DRM) += t736-gpu-power.o'),
    ('drivers/regulator/Makefile', 'obj-$(CONFIG_AW_DRM) += t736-gpu-supply.o'),
    ('arch/arm64/boot/dts/allwinner/Makefile', 'dtb-y += t736-gpu-candidate.dtb'),
]:
    p = src/file
    old = p.read_text()
    if line not in old:
        p.write_text(old+'\n'+line+'\n')
dts = src/'arch/arm64/boot/dts/allwinner'
gpu = (dts/'t736-gpu.dtsi').read_text()
# The integration node is deliberately disabled in the isolated board fragment.
if 'gpu: ' not in gpu and 'gpu:' not in gpu:
    raise SystemExit('Missing GPU node label')
(dts/'t736-gpu-candidate.dts').write_text(
    (dts/'t736-mainline-bringup.dts').read_text() +
    '\n#include "t736-hdmi.dtsi"\n#include "t736-gpu-supply.dtsi"\n#include "t736-gpu.dtsi"\n' +
    '\n&gpu { status = "okay"; };\n')
PY
"$src/scripts/config" --file "$out/.config" \
    --set-str LOCALVERSION '-t736-gpu-v15' --disable LOCALVERSION_AUTO \
    --enable REGULATOR --enable DEBUG_FS \
    --set-val CMA_SIZE_MBYTES 256
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
cp "$out/.config" "$root/userpatches/linux-t736-gpu-v15.config"
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    Image allwinner/t736-gpu-candidate.dtb modules
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
    KDEB_PKGVERSION=6.18.63-15 bindeb-pkg
sha256sum "$out/arch/arm64/boot/Image" \
    "$out/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb" \
    "$root/build/linux-image-6.18.63-t736-gpu-v15_6.18.63-15_arm64.deb"
