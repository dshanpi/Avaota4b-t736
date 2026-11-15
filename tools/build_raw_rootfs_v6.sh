#!/bin/sh
# Run as root in a private mount namespace. Does not access a board.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ "$(id -u)" = 0 ]
: "${T736_ROOT_PASSWORD:?Supply the board password in the process environment}"
stage="$root/build/armbian-raw-v6"
release=6.18.63-t736-armbian
image="$root/output/rootfs-armbian-raw-v6.ext4"
test ! -e "$stage"
test ! -e "$image"
cp -a --reflink=auto "$root/build/armbian-rootfs" "$stage"
mount --make-rprivate /
mount -t proc proc "$stage/proc"
mount --rbind /dev "$stage/dev"
mount -t tmpfs tmpfs "$stage/run"
cleanup() {
	umount -R "$stage/run" 2>/dev/null || true
	umount -R "$stage/dev" 2>/dev/null || true
	umount "$stage/proc" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$stage/tmp/t736-raw-build" "$stage/boot/dtb/allwinner" "$stage/etc/initramfs-tools/conf.d"
printf 'FSTYPE=ext4\n' > "$stage/etc/initramfs-tools/conf.d/t736-rootfs"
for revision in 4 6; do
	pkg="linux-image-${release}_6.18.63-${revision}_arm64.deb"
	cp "$root/build/$pkg" "$stage/tmp/t736-raw-build/"
	chroot "$stage" dpkg -i "/tmp/t736-raw-build/$pkg"
	cp "$stage/usr/lib/linux-image-$release/allwinner/t736-mainline-bringup.dtb" \
		"$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
	if [ "$revision" = 4 ]; then
		backup="$stage/boot/t736-v4-recovery"
		mkdir "$backup"
		cp -p "$stage/boot/vmlinuz-$release" "$stage/boot/initrd.img-$release" \
			"$stage/boot/config-$release" "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$backup/"
		(cd "$backup" && sha256sum vmlinuz-* initrd.img-* config-* *.dtb > SHA256SUMS)
		cp "$root/build/$pkg" "$backup/"
	fi
done
printf 'root:%s\n' "$T736_ROOT_PASSWORD" | chroot "$stage" /usr/sbin/chpasswd
unset T736_ROOT_PASSWORD
chroot "$stage" /usr/bin/chage -d "$(date +%F)" root
# The user already selected credentials. Keep the first-login marker as a
# disabled backup instead of reopening password setup after every reboot.
if [ -f "$stage/root/.not_logged_in_yet" ]; then
	mv "$stage/root/.not_logged_in_yet" "$stage/root/.not_logged_in_yet.raw-v6-disabled"
fi
mkdir -p "$stage/etc/systemd/network"
cat > "$stage/etc/systemd/network/20-wired.network" <<'EOF'
[Match]
Name=eth* end*
[Network]
DHCP=yes
EOF
systemctl --root="$stage" enable systemd-networkd.service systemd-resolved.service serial-getty@ttyS0.service
chroot "$stage" lsinitramfs "/boot/initrd.img-$release" > "$root/output/initrd-raw-v6.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$root/output/initrd-raw-v6.contents"
(cd "$stage/boot/t736-v4-recovery" && sha256sum -c SHA256SUMS)
test "$(chroot "$stage" dpkg-query -W -f='${Version}' "linux-image-$release")" = 6.18.63-6
cleanup
trap - EXIT HUP INT TERM
truncate -s 7802716160 "$image"
mkfs.ext4 -F -q -b 4096 -m 0 -L rootfs -O ^metadata_csum_seed,^orphan_file \
	-E root_owner=0:0 -d "$stage" "$image"
e2fsck -fn "$image"
printf 'RAW_V6_ROOTFS_WITH_V4_FALLBACK_OK\n'
