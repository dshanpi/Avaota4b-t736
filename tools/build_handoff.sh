#!/bin/sh
# Compile a RAM validation candidate. No persistent Boot1 update.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
cc=/home/ubuntu/t736-aiot/tina/brandy/brandy-2.0/tools/toolchain/gcc-linaro-7.2.1-2017.11-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-
out="$root/build/handoff"
mkdir -p "$out"
uboot="$root/build/u-boot-armbian/u-boot.bin"
dtb="$root/build/vendor-kernel.dtb"
"${cc}gcc" -march=armv8-a -marm -mfloat-abi=soft -Os -ffreestanding \
	-fno-builtin -fno-stack-protector -fno-pic -nostdlib -Wall -Wextra -Werror \
	-DUBOOT_BYTES="$(stat -c %s "$uboot")" -DDTB_BYTES="$(stat -c %s "$dtb")" \
	-Wl,--build-id=none,-T,"$root/handoff/handoff.ld" \
	"$root/handoff/start.S" "$root/handoff/handoff.c" -o "$out/handoff.elf"
"${cc}objcopy" -O binary "$out/handoff.elf" "$out/handoff.bin"
"${cc}objdump" -d "$out/handoff.elf" > "$out/handoff.disasm"
python3 "$root/tools/pack_handoff.py" \
	--header-source "$root/build/vendor-uboot-header.bin" \
	--shim "$out/handoff.bin" --uboot "$uboot" --dtb "$dtb" \
	--output "$root/output/t736-handoff-ram-v1.bin"
