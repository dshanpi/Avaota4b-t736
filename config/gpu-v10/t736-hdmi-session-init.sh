#!/bin/sh
# Apply the board's stable HDMI policy once after an Xorg session starts.
# Xorg calls the kernel card0-HDMI-A-1 output HDMI-1.
set -u

i=0
while [ "$i" -lt 30 ]; do
    if xrandr --query 2>/dev/null | grep -q '^HDMI-1 connected'; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

state=$(xrandr --query 2>/dev/null || true)
mode=$(printf '%s\n' "$state" | awk '
    $1 == "HDMI-1" && $2 == "connected" {
        for (i = 1; i <= NF; i++)
            if ($i ~ /^[0-9]+x[0-9]+\+/) { sub(/\+.*/, "", $i); print $i; exit }
    }')
if [ "$mode" != "1920x1080" ]; then
    xrandr --output HDMI-1 --mode 1920x1080 --rate 60 >/dev/null 2>&1 || true
fi

range=$(xrandr --verbose 2>/dev/null | awk '
    /^HDMI-1 connected/ { seen = 1 }
    seen && /^[[:space:]]*dynamicrange:/ { print $2; exit }')
if [ "$range" = "1" ]; then
    xrandr --output HDMI-1 --set dynamicrange 0 >/dev/null 2>&1 || true
fi
exit 0
