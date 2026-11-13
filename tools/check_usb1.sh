#!/bin/sh
# Read-only diagnostics. Does not mount or write attached USB storage.
set -u
uname -a
printf '\nDMA zones\n'
grep -E 'Node [0-9]+, zone|present|managed' /proc/zoneinfo
printf '\nBoard drivers\n'
for path in /sys/bus/platform/devices/2003300.clock-controller \
    /sys/bus/platform/devices/7025000.pinctrl \
    /sys/bus/platform/devices/4200800.phy \
    /sys/bus/platform/devices/4200000.usb \
    /sys/bus/platform/devices/4200400.usb; do
    printf '%s: ' "$path"
    readlink "$path/driver" || true
done
printf '\nDeferred probes\n'
cat /sys/kernel/debug/devices_deferred 2>/dev/null || true
printf '\nUSB devices\n'
for path in /sys/bus/usb/devices/*; do
    [ -r "$path/idVendor" ] || continue
    printf '%s\n' "$path"
    for field in idVendor idProduct manufacturer product speed bDeviceClass; do
        [ -r "$path/$field" ] || continue
        printf '  %s=' "$field"
        cat "$path/$field"
    done
done
printf '\nUSB topology\n'
if command -v lsusb >/dev/null 2>&1; then lsusb -t; fi
printf '\nClock and regulator state\n'
grep -E 't736-usb|usb1-vbus|vcc-pl' /sys/kernel/debug/clk/clk_summary \
    /sys/kernel/debug/regulator/regulator_summary 2>/dev/null || true
printf '\nUSB interrupts\n'
grep -E 'ehci|ohci|usb' /proc/interrupts || true
printf '\nKernel messages\n'
dmesg | grep -Ei 'usb|ehci|ohci|phy|pinctrl|regulator|dma|swiotlb'
printf '\nSystem services\n'
systemctl --failed --no-pager
