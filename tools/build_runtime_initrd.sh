#!/bin/sh
# Build a candidate initrd in a private mount namespace, preserving board data.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ "$(id -u)" = 0 ]
revision=${1:?package revision required}
case "$revision" in ''|*[!0-9]*|0) exit 1 ;; esac
stage="$root/build/armbian-rootfs"
package="linux-image-6.18.63-t736-armbian_6.18.63-$revision"_arm64.deb
[ -x "$stage/usr/lib/systemd/systemd" ]
[ -f "$root/build/$package" ]
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
mkdir -p "$stage/etc/initramfs-tools/conf.d"
printf 'FSTYPE=ext4\n' > "$stage/etc/initramfs-tools/conf.d/t736-rootfs"
cp "$root/build/$package" "$stage/tmp/$package"
chroot "$stage" /usr/bin/dpkg -i "/tmp/$package"
chroot "$stage" /usr/sbin/update-initramfs -u -k 6.18.63-t736-armbian
initrd="$stage/boot/initrd.img-6.18.63-t736-armbian"
listing="$root/evidence/build/initrd-v$revision.contents"
chroot "$stage" /usr/bin/lsinitramfs /boot/initrd.img-6.18.63-t736-armbian > "$listing"
for binary in fsck fsck.ext4; do
	grep -Eq "(^|/)(sbin|bin)/$binary$" "$listing"
done
cp "$initrd" "$root/output/initrd-t736-runtime-v$revision.img"
sha256sum "$root/output/initrd-t736-runtime-v$revision.img"
