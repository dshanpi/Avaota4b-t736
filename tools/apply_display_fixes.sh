#!/bin/sh
# Replay display fixes on the isolated Linux tree; never change the Tina donor.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
src="$root/sources/work/linux"
patchfile="$root/patches/t736-hdmi-sdr-baseline.patch"
if git -C "$src" apply --check "$patchfile" 2>/dev/null; then
    git -C "$src" apply "$patchfile"
else
    git -C "$src" apply --reverse --check "$patchfile"
fi
