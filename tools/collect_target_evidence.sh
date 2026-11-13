#!/bin/sh
# Read-only board-side collection; capture stdout on the host.
set -u
section() { printf '\n### %s\n' "$1"; }
section UTC
date -u
section KERNEL
uname -a
section IDENTITY
id
section CMDLINE
cat /proc/cmdline
section MEMORY
cat /proc/meminfo
section IOMEM
cat /proc/iomem
section CPU
cat /proc/cpuinfo
for item in online possible present; do
    printf '%s: ' "$item"
    cat "/sys/devices/system/cpu/$item" 2>/dev/null || true
done
section STORAGE
if command -v lsblk >/dev/null 2>&1; then
    lsblk -b -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS,UUID,PARTUUID 2>/dev/null ||
        lsblk -b -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,UUID,PARTUUID
fi
section MMC_SYSFS
for device in /sys/bus/mmc/devices/*; do
    [ -d "$device" ] || continue
    printf 'DEVICE %s\n' "$device"
    for property in name type cid csd ocr date manfid oemid fwrev hwrev; do
        [ -r "$device/$property" ] || continue
        printf '%s: ' "$property"
        cat "$device/$property"
    done
done
section MOUNTS
cat /proc/mounts
section MODEL
for property in model compatible; do
    if [ -r "/sys/firmware/devicetree/base/$property" ]; then
        printf '%s: ' "$property"
        tr '\000' '\n' < "/sys/firmware/devicetree/base/$property"
    fi
done
section EFFECTIVE_FDT
if [ -r /sys/firmware/fdt ] && command -v base64 >/dev/null 2>&1; then
    if command -v sha256sum >/dev/null 2>&1; then sha256sum /sys/firmware/fdt; fi
    printf 'BEGIN_FDT_BASE64\n'
    base64 /sys/firmware/fdt
    printf 'END_FDT_BASE64\n'
else
    printf 'Effective flattened DTB unavailable; capture it from bootloader/SDK separately.\n'
fi
section REGULATORS
if [ -r /sys/kernel/debug/regulator/regulator_summary ]; then
    cat /sys/kernel/debug/regulator/regulator_summary
fi
section CLOCKS
if [ -r /sys/kernel/debug/clk/clk_summary ]; then
    cat /sys/kernel/debug/clk/clk_summary
fi
section BOOT_LOG
dmesg 2>&1
section COLLECTION_END
printf 'Read-only collection completed. Missing/permission-denied fields still need investigation.\n'
