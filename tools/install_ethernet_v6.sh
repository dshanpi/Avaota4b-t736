#!/bin/sh
# File-level update only, after v5 I/O validation in this boot.
set -eu
release=6.18.63-t736-armbian
package=/tmp/t736-transfer/linux-ethernet-v6.deb
backup=/boot/t736-v5-recovery
test "$(id -u)" = 0
test "$(uname -r)" = "$release"
test "$(cat /sys/class/block/mmcblk0p4/start)" = 368640
test "$(cat /sys/class/block/mmcblk0p4/size)" = 15239680
test "$(findmnt -n -o SOURCE /)" = /dev/mmcblk0p4
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-5
cmp /proc/sys/kernel/random/boot_id /run/t736-pio-v5-validated
printf '717f9e6778884294a5563f21b003446af9d4f3d888a0aa3991db3ffa38a20b7a  %s\n' "$package" | sha256sum -c -
(cd /boot/t736-v4-recovery && sha256sum -c SHA256SUMS)
if [ ! -e "$backup" ]; then
	mkdir "$backup"
	cp -p "/boot/vmlinuz-$release" "/boot/initrd.img-$release" "/boot/config-$release" \
		/boot/dtb/allwinner/t736-mainline-bringup.dtb "$backup/"
	(cd "$backup" && sha256sum vmlinuz-* initrd.img-* config-* *.dtb > SHA256SUMS)
	sync
fi
(cd "$backup" && sha256sum -c SHA256SUMS)
cmp "/boot/vmlinuz-$release" "$backup/vmlinuz-$release"
cmp "/boot/initrd.img-$release" "$backup/initrd.img-$release"
cmp /boot/dtb/allwinner/t736-mainline-bringup.dtb "$backup/t736-mainline-bringup.dtb"
dpkg -i "$package"
test "$(dpkg-query -W -f='${Version}' linux-image-$release)" = 6.18.63-6
dtb="/usr/lib/linux-image-$release/allwinner/t736-mainline-bringup.dtb"
printf '020ee57ead39f4535a3945165ddb7eef513d1facdaa8496f62a9f6c387d5f2b4  %s\n' "$dtb" | sha256sum -c -
cp "$dtb" /boot/dtb/allwinner/t736-mainline-bringup.dtb.new
mv /boot/dtb/allwinner/t736-mainline-bringup.dtb.new /boot/dtb/allwinner/t736-mainline-bringup.dtb
printf '47ffe1952daebd3f04cbea294a4c8ef251c122dfc8a2efac29d1984361ff9d4f  /boot/vmlinuz-%s\n' "$release" | sha256sum -c -
lsinitramfs "/boot/initrd.img-$release" > /tmp/t736-transfer/initrd-v6.contents
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' /tmp/t736-transfer/initrd-v6.contents
sha256sum "/boot/vmlinuz-$release" "/boot/initrd.img-$release" /boot/dtb/allwinner/t736-mainline-bringup.dtb
sync
printf 'T736_ETHERNET_V6_INSTALL_OK\n'
