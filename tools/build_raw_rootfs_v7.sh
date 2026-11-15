#!/bin/sh
# Build a complete filesystem from the authenticated v6 staging tree.
# Run as root in a private mount namespace; never accesses the board.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ "$(id -u)" = 0 ]
stage="$root/build/armbian-raw-v7"
release=6.18.63-t736-armbian
image="$root/output/rootfs-armbian-raw-v7.ext4"
test ! -e "$stage"
test ! -e "$image"
cp -a --reflink=auto "$root/build/armbian-raw-v6" "$stage"
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
backup="$stage/boot/t736-v6-recovery"
mkdir "$backup"
cp -p "$stage/boot/vmlinuz-$release" "$stage/boot/initrd.img-$release" \
	"$stage/boot/config-$release" "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$backup/"
(cd "$backup" && sha256sum vmlinuz-* initrd.img-* config-* *.dtb > SHA256SUMS)
cp "$root/build/linux-image-${release}_6.18.63-6_arm64.deb" "$backup/"
pkg="linux-image-${release}_6.18.63-7_arm64.deb"
cp "$root/build/$pkg" "$stage/tmp/t736-raw-build/"
chroot "$stage" dpkg -i "/tmp/t736-raw-build/$pkg"
cp "$stage/usr/lib/linux-image-$release/allwinner/t736-mainline-bringup.dtb" \
	"$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
# Preserve the user's existing credentials without exposing the shadow hash.
cmp -s "$root/build/armbian-raw-v6/etc/shadow" "$stage/etc/shadow"
chroot "$stage" lsinitramfs "/boot/initrd.img-$release" > "$root/output/initrd-raw-v7.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$root/output/initrd-raw-v7.contents"
for revision in 4 6; do
	(cd "$stage/boot/t736-v$revision-recovery" && sha256sum -c SHA256SUMS)
done
test "$(chroot "$stage" dpkg-query -W -f='${Version}' "linux-image-$release")" = 6.18.63-7
cleanup
trap - EXIT HUP INT TERM
truncate -s 7802716160 "$image"
mkfs.ext4 -F -q -b 4096 -m 0 -L rootfs -O ^metadata_csum_seed,^orphan_file \
	-E root_owner=0:0 -d "$stage" "$image"
e2fsck -fn "$image"
printf 'RAW_V7_ROOTFS_WITH_V4_V6_FALLBACK_OK\n'
