#!/bin/sh
set -eu
test "$(id -u)" = 0
test "$(uname -r)" = 6.18.63-t736-hdmi-v13
test "$(cat /proc/sys/kernel/tainted)" = 0
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
test "$(systemctl --failed --no-legend --plain | wc -l)" = 0
test "$(cat /sys/class/drm/card0-HDMI-A-1/status)" = connected
test "$(cat /sys/class/drm/card0-HDMI-A-1/enabled)" = enabled
test "$(cat /sys/class/graphics/fb0/name)" = t736-drm-fb
test "$(cat /sys/class/vtconsole/vtcon1/bind)" = 1
systemctl is-active getty@tty1.service
sha256sum -c /root/t736-source-v6-io-check.sha256
sfdisk --verify /dev/mmcblk0
if dmesg | grep -E 'Oops:|Kernel panic|Unable to handle kernel|GPT:.*(not|error|mismatch)'; then
    exit 2
fi
printf 'V6_REBOOT_STATE\n'
uname -r
cat /proc/cmdline /proc/sys/kernel/random/boot_id /proc/uptime /proc/sys/kernel/tainted
cat /sys/devices/system/cpu/online
free -m
findmnt /
ip -br address
cat /sys/class/net/end0/speed /sys/class/net/end0/duplex
ip -s link show end0
ping -c 3 -W 2 10.0.0.1
systemctl --failed --no-pager
grep -E 'tcon|display-engine' /proc/interrupts
grep -E 'active=|irqcnt:|vsync:' /sys/kernel/debug/dri/0/state
printf 'SOURCE_V6_REBOOT_ACCEPTANCE_OK\n'
