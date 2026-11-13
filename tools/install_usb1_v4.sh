#!/bin/sh
# One-board validation update; never writes a partition table or raw block device.
set -eu
release=6.18.63-t736-armbian
package=/tmp/t736-transfer/linux-usb-v4.deb
backup=/boot/t736-v3-recovery
test "$(id -u)" = 0
test "$(uname -r)" = "$release"
test "$(cat /sys/class/block/mmcblk0p4/start)" = 368640
test "$(cat /sys/class/block/mmcblk0p4/size)" = 15239680
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-3
printf 'edebf4e704ee8716a5b8b90458fda32adf234bb3a2f5c98d0c1882a69b5ab3fe  %s\n' "$package" | sha256sum -c -
test ! -e "$backup"
mkdir "$backup"
cp -p "/boot/vmlinuz-$release" "$backup/"
cp -p "/boot/initrd.img-$release" "$backup/"
cp -p "/boot/config-$release" "$backup/"
cp -p /boot/dtb/allwinner/t736-mainline-bringup.dtb "$backup/"
(cd "$backup" && sha256sum vmlinuz-* initrd.img-* config-* *.dtb > SHA256SUMS)
sync
(cd "$backup" && sha256sum -c SHA256SUMS)
dpkg -i "$package"
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-4
dtb="/usr/lib/linux-image-$release/allwinner/t736-mainline-bringup.dtb"
printf '40ece1d8d9682477bdc892817696f93b5a162015e79b47a9b24c7cb72c39450a  %s\n' "$dtb" | sha256sum -c -
cp "$dtb" /boot/dtb/allwinner/t736-mainline-bringup.dtb.new
mv /boot/dtb/allwinner/t736-mainline-bringup.dtb.new /boot/dtb/allwinner/t736-mainline-bringup.dtb
lsinitramfs "/boot/initrd.img-$release" > /tmp/t736-transfer/initrd-v4.contents
grep -E '(^|/)(sbin|bin)/fsck(\.ext4)?$' /tmp/t736-transfer/initrd-v4.contents
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' /tmp/t736-transfer/initrd-v4.contents
sha256sum "/boot/vmlinuz-$release" "/boot/initrd.img-$release" /boot/dtb/allwinner/t736-mainline-bringup.dtb
sync
printf '\nT736_USB1_V4_INSTALL_OK\n'
