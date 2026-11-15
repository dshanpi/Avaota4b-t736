#!/bin/sh
# Verified file-level update. v4 boot files must already be saved on the board.
set -eu
release=6.18.63-t736-armbian
package=/tmp/t736-transfer/linux-pio-v5.deb
backup=/boot/t736-v4-recovery
test "$(id -u)" = 0
test "$(uname -r)" = "$release"
test "$(cat /sys/class/block/mmcblk0p4/start)" = 368640
test "$(cat /sys/class/block/mmcblk0p4/size)" = 15239680
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-4
printf '06bda18a5365f090404e8b53213970c676fd6bbfb07ee5c6760676492e32eb3c  %s\n' "$package" | sha256sum -c -
(cd "$backup" && sha256sum -c SHA256SUMS)
cmp "/boot/vmlinuz-$release" "$backup/vmlinuz-$release"
cmp "/boot/initrd.img-$release" "$backup/initrd.img-$release"
cmp /boot/dtb/allwinner/t736-mainline-bringup.dtb "$backup/t736-mainline-bringup.dtb"
dpkg -i "$package"
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-5
dtb="/usr/lib/linux-image-$release/allwinner/t736-mainline-bringup.dtb"
printf '6a610cc0b3750fb14e1e1db8cfb011ae8d8b90638273898c2a504467e5b0824a  %s\n' "$dtb" | sha256sum -c -
cp "$dtb" /boot/dtb/allwinner/t736-mainline-bringup.dtb.new
mv /boot/dtb/allwinner/t736-mainline-bringup.dtb.new /boot/dtb/allwinner/t736-mainline-bringup.dtb
printf '1441eef3fcff68f6b9a81d26e701c04c421b072eda97c046913eb26110debf68  /boot/vmlinuz-%s\n' "$release" | sha256sum -c -
lsinitramfs "/boot/initrd.img-$release" > /tmp/t736-transfer/initrd-v5.contents
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' /tmp/t736-transfer/initrd-v5.contents
sha256sum "/boot/vmlinuz-$release" "/boot/initrd.img-$release" /boot/dtb/allwinner/t736-mainline-bringup.dtb
sync
printf '\nT736_PIO_V5_INSTALL_OK\n'
