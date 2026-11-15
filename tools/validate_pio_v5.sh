#!/bin/sh
# Bounded filesystem I/O only: no writes to raw block devices.
set -eu
test "$(id -u)" = 0
test "$(dpkg-query -W -f='${Version}' linux-image-6.18.63-t736-armbian)" = 6.18.63-5
uname -a
grep -q '#5 SMP' /proc/version
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
test "$(cat /sys/class/block/mmcblk0p4/start)" = 368640
test "$(cat /sys/class/block/mmcblk0p4/size)" = 15239680
findmnt /
pins=/sys/kernel/debug/pinctrl/2000000.pinctrl/pinmux-pins
for pin in PC0 PC1 PC5 PC6 PC8 PC9 PC10 PC11 PC13 PC14 PC15 PC16; do
	grep "($pin):.*4023000.mmc" "$pins"
done
printf 'PIO_PC_OWNERSHIP_OK\n'
cat /sys/kernel/debug/mmc0/ios
cat /sys/kernel/debug/devices_deferred
systemctl --failed --no-pager
(cd /boot/t736-v4-recovery && sha256sum -c SHA256SUMS)
scratch=$(mktemp /var/tmp/t736-v5-io.XXXXXX)
source=$(mktemp /run/t736-v5-source.XXXXXX)
trap 'rm -f "$scratch" "$source"' EXIT HUP INT TERM
test "$(findmnt -n -o SOURCE -T "$scratch")" = /dev/mmcblk0p4
dd if=/dev/urandom of="$source" bs=1M count=16 status=none
expected=$(sha256sum "$source" | cut -d ' ' -f 1)
dd if="$source" of="$scratch" bs=1M conv=fsync
# O_DIRECT readback bypasses the file data cache.
actual=$(dd if="$scratch" bs=1M iflag=direct | sha256sum | cut -d ' ' -f 1)
printf 'expected=%s\nreadback=%s\n' "$expected" "$actual"
test "$actual" = "$expected"
sync
printf 'PIO_V5_16M_DIRECT_READBACK_OK\n'
dmesg | tail -35
cat /proc/sys/kernel/random/boot_id > /run/t736-pio-v5-validated
printf 'T736_PIO_V5_VALIDATION_OK\n'
