#!/bin/sh
# Compile-only experiment. Does not alter the default kernel tree or the board.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux-pio-audit"
out="$root/build/linux-pio-audit"
patchfile="$root/patches/linux/0002-t736-main-pio-layout.patch"
if [ ! -d "$src" ]; then
	cp -a --reflink=auto "$root/sources/work/linux" "$src"
fi
if patch --dry-run --forward -d "$src" -p1 < "$patchfile" >/dev/null 2>&1; then
	patch --forward -d "$src" -p1 < "$patchfile"
else
	# Accept only this exact already-applied patch, not an arbitrary failed apply.
	patch --dry-run --reverse -d "$src" -p1 < "$patchfile" >/dev/null
fi
cp "$root/boards/pinctrl-t736.c" "$src/drivers/pinctrl/sunxi/"
python3 "$root/tools/check_pio_layout.py" "$src"
mkdir -p "$out"
cp "$root/build/linux-armbian/.config" "$out/.config"
make -s -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
	olddefconfig prepare scripts
make -s -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 \
	drivers/pinctrl/sunxi/pinctrl-sunxi.o \
	drivers/pinctrl/sunxi/pinctrl-t736.o \
	drivers/pinctrl/sunxi/pinctrl-t736-r.o
printf 'PIO_CANDIDATE_COMPILE_PASS\n'
