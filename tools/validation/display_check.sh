#!/bin/sh
# Read-only display and board-state evidence for the isolated HDMI candidate.
uname -a
cat /proc/cmdline
printf '\nDISPLAY_KERNEL_LOG\n'
dmesg | grep -Ei 'drm|hdmi|tcon|display|framebuffer|fb0|pll|power-on|cma|deferred|fail|error|oops|panic'
printf '\nDISPLAY_DEVICES\n'
ls -l /dev/dri /dev/fb* /sys/class/graphics 2>/dev/null
for connector in /sys/class/drm/card*-*; do
    [ -f "$connector/status" ] || continue
    printf '\nCONNECTOR %s\n' "$connector"
    cat "$connector/status" "$connector/enabled" "$connector/modes" 2>/dev/null
    wc -c "$connector/edid"
    sha256sum "$connector/edid"
done
for fb in /sys/class/graphics/fb*; do
    [ -d "$fb" ] || continue
    printf '\nFRAMEBUFFER %s\n' "$fb"
    cat "$fb/name" "$fb/virtual_size" "$fb/bits_per_pixel" "$fb/mode" 2>/dev/null
done
printf '\nDISPLAY_CLOCKS\n'
grep -E 't736-de|t736-vo|t736-tv|t736-hdmi|t736-display' /sys/kernel/debug/clk/clk_summary
printf '\nPOWER_DOMAINS\n'
cat /sys/kernel/debug/pm_genpd/pm_genpd_summary
printf '\nDEFERRED_DEVICES\n'
cat /sys/kernel/debug/devices_deferred
printf '\nDRM_STATE\n'
for state in /sys/kernel/debug/dri/*/state; do
    [ -f "$state" ] && cat "$state"
done
printf '\nDISPLAY_INTERRUPTS\n'
grep -Ei 'drm|hdmi|tcon|display|5000000|5730000|5520000' /proc/interrupts
printf '\nSYSTEM_STATE\n'
cat /proc/sys/kernel/tainted
systemctl --failed --no-pager
ip -br address
printf '\nDISPLAY_CHECK_FINISHED\n'
