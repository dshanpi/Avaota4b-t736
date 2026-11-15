#!/bin/sh
# Run as root in a private mount namespace. Original v1 filesystem stays intact.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ "$(id -u)" = 0 ]
image="$root/output/rootfs-armbian-source-v5.ext4"
stage="$root/build/source-rootfs-v5-mount"
old=6.18.63-t736-armbian
release=6.18.63-t736-hdmi-v12
pkg="linux-image-${release}_6.18.63-12_arm64.deb"
test ! -e "$image"
test ! -e "$stage"
test -s "$root/build/$pkg"
cp --reflink=auto --sparse=always "$root/output/rootfs-armbian-source-v1.ext4" "$image"
mkdir "$stage"
mount --make-rprivate /
mount -o loop "$image" "$stage"
cleanup() {
    umount -R "$stage/run" 2>/dev/null || true
    umount -R "$stage/dev" 2>/dev/null || true
    umount "$stage/proc" 2>/dev/null || true
    umount "$stage" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
shadow_before=$(sha256sum "$stage/etc/shadow")
backup="$stage/boot/t736-source-v4-recovery"
mkdir "$backup"
cp -p "$stage/boot/vmlinuz-$old" "$stage/boot/initrd.img-$old" \
    "$stage/boot/config-$old" "$stage/boot/System.map-$old" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$backup/"
(cd "$backup" && sha256sum vmlinuz-* initrd.img-* config-* System.map-* *.dtb > SHA256SUMS)
mount -t proc proc "$stage/proc"
mount --rbind /dev "$stage/dev"
mount -t tmpfs tmpfs "$stage/run"
mkdir -p "$stage/tmp/t736-raw-build"
cp "$root/build/$pkg" "$stage/tmp/t736-raw-build/"
chroot "$stage" dpkg -i "/tmp/t736-raw-build/$pkg"
chroot "$stage" dpkg -r "linux-image-$old"
# Source U-Boot v1 uses these fixed filenames. Keep compatibility aliases.
ln -s "vmlinuz-$release" "$stage/boot/vmlinuz-$old"
ln -s "initrd.img-$release" "$stage/boot/initrd.img-$old"
cp "$stage/usr/lib/linux-image-$release/allwinner/t736-hdmi-candidate.dtb" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
test "$shadow_before" = "$(sha256sum "$stage/etc/shadow")"
chroot "$stage" lsinitramfs "/boot/initrd.img-$release" > "$root/output/initrd-source-v5.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$root/output/initrd-source-v5.contents"
test "$(chroot "$stage" dpkg-query -W -f='${Version}' "linux-image-$release")" = 6.18.63-12
cmp "$root/build/linux-hdmi-v12/arch/arm64/boot/dts/allwinner/t736-hdmi-candidate.dtb" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
gzip -dc "$stage/boot/vmlinuz-$release" | cmp - "$root/build/linux-hdmi-v12/arch/arm64/boot/Image"
(cd "$backup" && sha256sum -c SHA256SUMS)
sync
cleanup
trap - EXIT HUP INT TERM
e2fsck -fn "$image"
printf 'SOURCE_V5_ROOTFS_READY\n'
