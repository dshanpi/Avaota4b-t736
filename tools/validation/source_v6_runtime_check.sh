#!/bin/sh
set -eu
test "$(id -u)" = 0
test "$(uname -r)" = 6.18.63-t736-hdmi-v13
test "$(cat /proc/sys/kernel/tainted)" = 0
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
findmnt -n -o OPTIONS / | grep -Eq '(^|,)rw(,|$)'
test "$(cat /sys/block/mmcblk0/size)" = 122142720
test "$(cat /sys/block/mmcblk0/device/type)" = MMC
sha256sum -c <<'HASHES'
35889f79f5f28447ac2a2f7bf885fa93a767ad7aecfa00f3c9c66a9cb912f03d  /boot/vmlinuz-6.18.63-t736-hdmi-v13
7bbcde44b6d07e35f9ff9549f215f34584784f089947c67cd8df352941d04662  /boot/initrd.img-6.18.63-t736-hdmi-v13
d81986f7035f252583ae30f861dc3e55675d296547f12704ba81678fd7fe03ed  /boot/dtb/allwinner/t736-mainline-bringup.dtb
HASHES
test "$(dd if=/dev/mmcblk0 bs=512 skip=16 count=432 status=none | sha256sum | cut -d' ' -f1)" = a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea
(cd /boot/t736-source-v4-recovery && sha256sum -c SHA256SUMS)
f=/root/t736-source-v6-io-check.bin
test ! -e "$f"
dd if=/dev/urandom of="$f" bs=1M count=16 conv=fsync status=none
expected=$(sha256sum "$f" | cut -d' ' -f1)
actual=$(dd if="$f" bs=1M count=16 iflag=direct status=none | sha256sum | cut -d' ' -f1)
test "$expected" = "$actual"
printf '%s  %s\n' "$expected" "$f" > /root/t736-source-v6-io-check.sha256
sync
printf 'SOURCE_V6_IO_16M_FSYNC_DIRECT_READ_OK %s\n' "$actual"
test "$(systemctl --failed --no-legend --plain | wc -l)" = 0
systemctl --failed --no-pager
cat /sys/devices/system/cpu/online
free -m
ip -br address
ip route
cat /sys/kernel/debug/mmc0/ios
test "$(cat /sys/class/drm/card0-HDMI-A-1/status)" = connected
test "$(cat /sys/class/drm/card0-HDMI-A-1/enabled)" = enabled
test "$(cat /sys/class/graphics/fb0/name)" = t736-drm-fb
systemctl is-active getty@tty1.service
cat /proc/fb
cat /sys/class/vtconsole/vtcon*/name /sys/class/vtconsole/vtcon*/bind
printf 'SOURCE_V6_RUNTIME_OK\n'
