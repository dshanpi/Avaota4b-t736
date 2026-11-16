#!/bin/sh
# Run only on the v15 candidate; no display mode changes in this helper.
set -eu
[ "$(id -u)" = 0 ]
[ "$(uname -r)" = 6.18.63-t736-gpu-v15 ]
phase=${1:-status}
case "$phase" in status|gbm) ;; *) exit 2 ;; esac
printf 'KERNEL=%s\n' "$(uname -r)"
if [ "$phase" = gbm ]; then
    modprobe pvrsrvkm
    udevadm settle --timeout=10
fi
printf 'DRM_DEVICES\n'
ls -l /dev/dri
printf 'GPU_CLOCKS\n'
grep -E 't736-gpu|t736-display-ppu' /sys/kernel/debug/clk/clk_summary || true
printf 'GPU_SUPPLY\n'
grep -E 'dcdc5|gpu' /sys/kernel/debug/regulator/regulator_summary || true
printf 'GPU_DRIVER_LOG\n'
dmesg | grep -Ei 'pvr|rogue|t736-gpu|dcdc5|firmware.*rgx|gpu.*fail' | tail -n 45 || true
if [ "$phase" = gbm ]; then
    node=
    for entry in /sys/class/drm/renderD*; do
        [ -e "$entry" ] || continue
        module=$(basename "$(readlink -f "$entry/device/driver/module")")
        if [ "$module" = pvrsrvkm ]; then
            [ -z "$node" ] || { printf 'Ambiguous GPU render nodes\n' >&2; exit 1; }
            node=/dev/dri/$(basename "$entry")
        fi
    done
    [ -n "$node" ] || { printf 'No pvrsrvkm render node\n' >&2; exit 1; }
    printf 'TEST_RENDER_NODE=%s\n' "$node"
    timeout --kill-after=3s 30s /usr/bin/t736-img-run probe-gbm "$node"
    printf 'POST_RENDER_LOG\n'
    dmesg | grep -Ei 'pvr|rogue|firmware.*rgx|gpu.*fail|MMU.*fault|HWR' | tail -n 45 || true
fi
