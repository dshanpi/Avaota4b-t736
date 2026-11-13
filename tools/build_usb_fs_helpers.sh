#!/bin/sh
# Temporary FAT charset modules for the installed v4 kernel; official sources.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux"
kernel="$root/build/linux-armbian"
out="$root/build/usb-fs-helpers-v4"
[ -s "$kernel/Module.symvers" ]
grep -q '^CONFIG_MODULES=y$' "$kernel/.config"
grep -q '^CONFIG_NLS=y$' "$kernel/.config"
mkdir -p "$out"
cp "$src/fs/nls/nls_cp437.c" "$src/fs/nls/nls_iso8859-1.c" "$out/"
printf 'obj-m += nls_cp437.o nls_iso8859-1.o\n' > "$out/Makefile"
make -C "$src" O="$kernel" M="$out" ARCH=arm64 \
	CROSS_COMPILE=aarch64-linux-gnu- modules
sha256sum "$out/nls_cp437.ko" "$out/nls_iso8859-1.ko"
