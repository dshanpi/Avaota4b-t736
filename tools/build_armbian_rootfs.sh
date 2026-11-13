#!/bin/sh
# Run as root inside a private mount namespace.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ "$(id -u)" = 0 ]
stage="$root/build/armbian-rootfs"
revision="${KERNEL_PACKAGE_REVISION:-1}"
case "$revision" in 1|2) ;; *) exit 1 ;; esac
[ -x "$stage/usr/lib/systemd/systemd" ]
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
cp /etc/resolv.conf "$stage/etc/resolv.conf"
mkdir -p "$stage/tmp/t736-packages-$revision"
cp "$root/build/linux-image-6.18.63-t736-armbian_6.18.63-$revision"_arm64.deb "$stage/tmp/t736-packages-$revision/"
cp "$root"/sources/work/armbian-build/output/debs/armbian-bsp-cli-t736-mainline*_arm64__*.deb "$stage/tmp/t736-packages-$revision/"
chroot "$stage" /usr/bin/apt-get update
chroot "$stage" /bin/sh -c "DEBIAN_FRONTEND=noninteractive apt-get -y install /tmp/t736-packages-$revision/*.deb"
printf 't736\n' > "$stage/etc/hostname"
cat > "$stage/etc/hosts" <<'EOF'
127.0.0.1 localhost
127.0.1.1 t736
::1 localhost ip6-localhost ip6-loopback
EOF
cat > "$stage/etc/fstab" <<'EOF'
PARTUUID=a0085546-4166-744a-a353-fca9272b8e48 / ext4 defaults,noatime 0 1
tmpfs /tmp tmpfs defaults,nosuid 0 0
EOF
# Standard Armbian first-login password; force replacement on first login.
printf 'root:1234\n' | chroot "$stage" /usr/sbin/chpasswd
chroot "$stage" /usr/bin/chage -d 0 root
touch "$stage/root/.not_logged_in_yet"
systemctl --root="$stage" enable serial-getty@ttyS0.service
systemctl --root="$stage" set-default multi-user.target
mkdir -p "$stage/boot/dtb/allwinner"
cp "$root/output/t736-mainline-armbian.dtb" "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
cp "$root/build/linux-armbian/.config" "$stage/boot/config-6.18.63-t736-armbian"
chroot "$stage" /usr/sbin/update-initramfs -u -k 6.18.63-t736-armbian
chroot "$stage" /usr/bin/apt-get clean
cleanup
trap - EXIT HUP INT TERM
mkdir -p "$root/output"
image="$root/output/rootfs-armbian-t736-v$revision.ext4"
[ ! -e "$image" ]
truncate -s 7802716160 "$image"
mkfs.ext4 -F -q -b 4096 -m 0 -L rootfs -O ^metadata_csum_seed,^orphan_file \
	-E root_owner=0:0 -d "$stage" "$image"
e2fsck -fn "$image"
/home/ubuntu/t736-aiot/tina/tools/pack/pctools/linux/android/img2simg \
	"$image" "$root/output/rootfs-armbian-t736-v$revision.sparse"
sha256sum "$root/output/rootfs-armbian-t736-v$revision.sparse"
