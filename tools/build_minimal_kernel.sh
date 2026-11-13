#!/bin/sh
# Build the locked official kernel with the initial UART/PSCI initramfs profile.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
python3 - "$root" <<'PY'
import sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / 'tools'))
from workspace_policy import outside_tina
outside_tina(root)
assert root == Path('/home/ubuntu/t736-mainline')
PY
src="$root/sources/snapshots/linux/linux-6.18.63"
out="$root/build/linux-bringup"
mkdir -p "$out" "$root/evidence/build"
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- tinyconfig
cfg="$src/scripts/config"
for symbol in PRINTK BUG BINFMT_ELF BLK_DEV_INITRD RD_GZIP DEVTMPFS DEVTMPFS_MOUNT \
    PROC_FS SYSFS TMPFS FUTEX FILE_LOCKING POSIX_TIMERS MULTIUSER \
    OF COMMON_CLK ARM_GIC_V3 ARM_PSCI_FW ARM_ARCH_TIMER SMP HOTPLUG_CPU \
    SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_8250_DW SERIAL_OF_PLATFORM \
    SERIAL_EARLYCON TTY UNIX98_PTYS VT UNIX NET INET BLOCK EXT4_FS \
    MMC MMC_BLOCK DEBUG_KERNEL KALLSYMS; do
    "$cfg" --file "$out/.config" --enable "$symbol"
done
"$cfg" --file "$out/.config" --set-val NR_CPUS 8 \
    --set-val SERIAL_8250_NR_UARTS 1 --set-val SERIAL_8250_RUNTIME_UARTS 1 \
    --set-str LOCALVERSION '-t736-bringup' --disable LOCALVERSION_AUTO \
    --set-str CMDLINE 'earlycon=uart8250,mmio32,0x02500000 console=ttyS0,115200 rdinit=/init loglevel=8 clk_ignore_unused' \
    --enable CMDLINE_FORCE
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$src" O="$out" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image
cp "$out/arch/arm64/boot/Image" "$root/output/Image-6.18.63-t736-bringup"
sha256sum "$root/output/Image-6.18.63-t736-bringup"
