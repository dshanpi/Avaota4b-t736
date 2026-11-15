#!/bin/bash
set -eu
test "$(cat /sys/block/mmcblk0/size)" = 122142720
test "$(cat /sys/block/mmcblk0/device/type)" = MMC
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
b=/root/t736-source-v6-gpt-before
test ! -e "$b"
mkdir -m 700 "$b"
sfdisk --dump /dev/mmcblk0 > "$b/layout.sfdisk"
dd if=/dev/mmcblk0 of="$b/primary.bin" bs=512 count=16 status=none
dd if=/dev/mmcblk0 of="$b/vendor-mirror.bin" bs=512 skip=40960 count=16 status=none
dd if=/dev/mmcblk0 of="$b/old-tail.bin" bs=512 skip=16777182 count=34 status=none
dd if=/dev/mmcblk0 of="$b/device-tail.bin" bs=512 skip=122142686 count=34 status=none
sha256sum "$b"/*.bin
sync
sfdisk --no-reread --no-tell-kernel --relocate gpt-bak-std /dev/mmcblk0
sfdisk --dump /dev/mmcblk0 > "$b/layout-after.sfdisk"
diff -u <(sed -n '/^\/dev\/mmcblk0p/p' "$b/layout.sfdisk") <(sed -n '/^\/dev\/mmcblk0p/p' "$b/layout-after.sfdisk")
sfdisk --verify /dev/mmcblk0
dd if=/dev/mmcblk0 of=/run/t736-source-v6-primary-gpt.bin bs=512 count=16 status=none
dd if=/run/t736-source-v6-primary-gpt.bin of=/dev/mmcblk0 bs=512 seek=40960 count=16 conv=notrunc,fsync status=none
sync
printf 'GPT_RELOCATED_PARTITIONS_UNCHANGED\n'
printf 'GPT_primary_BEGIN\n'
dd if=/dev/mmcblk0 bs=512 count=16 status=none | base64
printf 'GPT_primary_END\nGPT_mirror_BEGIN\n'
dd if=/dev/mmcblk0 bs=512 skip=40960 count=16 status=none | base64
printf 'GPT_mirror_END\nGPT_tail_BEGIN\n'
dd if=/dev/mmcblk0 bs=512 skip=122142717 count=3 status=none | base64
printf 'GPT_tail_END\n'
