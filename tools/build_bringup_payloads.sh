#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
stage="$root/build/initramfs-stage"
mkdir -p "$stage/bin" "$stage/sbin" "$stage/usr/bin" "$stage/usr/sbin" \
	"$stage/dev" "$stage/proc" "$stage/sys" "$stage/tmp" "$stage/run" "$root/output"
cp "$root/build/busybox/busybox" "$stage/bin/busybox"
for app in sh mount mkdir setsid cttyhack uname cat head; do
	ln -sfn busybox "$stage/bin/$app"
done
cp "$root/initramfs/init" "$stage/init"
chmod 0755 "$stage/init" "$stage/bin/busybox"
spec="$root/build/initramfs.list"
printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n' > "$spec"
find "$stage" -mindepth 1 -printf '%P\n' | sort | while IFS= read -r name; do
	[ "$name" != dev ] || continue
	if [ -L "$stage/$name" ]; then
		printf 'slink /%s %s 0777 0 0\n' "$name" "$(readlink "$stage/$name")"
	elif [ -d "$stage/$name" ]; then
		printf 'dir /%s 0755 0 0\n' "$name"
	else
		printf 'file /%s %s 0755 0 0\n' "$name" "$stage/$name"
	fi
done >> "$spec"
"$root/build/linux-bringup/usr/gen_init_cpio" -t 0 "$spec" > "$root/build/initramfs.cpio"
gzip -n -9 -c "$root/build/initramfs.cpio" > "$root/output/initramfs-t736-bringup.cpio.gz"
dtc -I dts -O dtb -o "$root/output/t736-mainline-bringup.dtb" "$root/boards/t736-mainline-bringup.dts"
sha256sum "$root/output/initramfs-t736-bringup.cpio.gz" "$root/output/t736-mainline-bringup.dtb"
