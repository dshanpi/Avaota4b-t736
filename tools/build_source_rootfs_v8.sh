#!/bin/sh
# Offline v8 candidate only. Usage: sudo -n sh tools/build_source_rootfs_v8.sh /absolute/pvrsrvkm.ko
set -eu
[ "$#" -eq 1 ] || { printf 'Usage: %s /absolute/pvrsrvkm.ko\n' "$0" >&2; exit 2; }
[ "$(id -u)" = 0 ]
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
apt_mirror=${T736_V8_APT_MIRROR:-default}
case "$apt_mirror" in default|aliyun) ;; *) exit 2 ;; esac
case "$1" in /*.ko) ;; *) printf 'An absolute uncompressed .ko path is required\n' >&2; exit 2 ;; esac
if [ "${T736_V8_IN_NAMESPACE:-0}" != 1 ]; then
    exec unshare --mount --propagation private env T736_V8_IN_NAMESPACE=1 \
        T736_V8_CALLER_NS="$(readlink /proc/self/ns/mnt)" sh "$0" "$1"
fi
[ "$(readlink /proc/self/ns/mnt)" != "${T736_V8_CALLER_NS:-}" ]
mount --make-rprivate /
mkdir -p "$root/build" "$root/output" "$root/evidence"
exec 9> "$root/build/source-rootfs-v8.lock"
flock -n 9
module=$(realpath -e -- "$1")
release=6.18.63-t736-gpu-v15
previous=6.18.63-t736-hdmi-v14
compat=6.18.63-t736-armbian
base="$root/output/rootfs-armbian-source-v7.ext4"
image="$root/output/rootfs-armbian-source-v8.ext4"
stage="$root/build/source-rootfs-v8-mount"
state="$root/build/source-rootfs-v8-state"
kernel="$root/build/linux-image-${release}_6.18.63-15_arm64.deb"
gpu="$root/build/gpu-userspace/t736-img-userspace-candidate_24.2.6603887-1_arm64.deb"
gpu_sha=9d8da37a1e7b9986c71ef210712acbefba0f778984f355d347575e510e871ecf
gpu_prefix=/opt/t736-img-ddk-24.2-x11
monitor_config="$root/config/gpu-v8/monitors.xml"
test -f "$module"
test "$(modinfo -F name "$module")" = pvrsrvkm
case "$(modinfo -F vermagic "$module")" in "$release "*) ;; *) printf 'Module vermagic mismatch\n' >&2; exit 1 ;; esac
strings -a "$module" | grep -Fx 'Rogue_DDK_Linux_WS rogueddk 24.2@6603887' >/dev/null || \
    { printf 'Module DDK mismatch\n' >&2; exit 1; }
test -s "$kernel"
test "$(dpkg-deb -f "$kernel" Package)" = "linux-image-$release"
test "$(dpkg-deb -f "$kernel" Architecture)" = arm64
test "$(dpkg-deb -f "$kernel" Version)" = 6.18.63-15
printf '%s  %s\n' "$gpu_sha" "$gpu" | sha256sum -c -
test "$(dpkg-deb -f "$gpu" Package)" = t736-img-userspace-candidate
test "$(dpkg-deb -f "$gpu" Architecture)" = arm64
test -s "$monitor_config"
test -f "$root/build/source-rootfs-v7-state/finalize.complete"
sha256sum -c --status "$base.sha256"
test "$(stat -c %s "$base")" = 7802716160
test ! -e "$image" && test ! -L "$image"
test ! -e "$state" && test ! -L "$state"
test ! -L "$stage"
mkdir -p "$stage"
test -z "$(find "$stage" -mindepth 1 -maxdepth 1 -print -quit)"
mkdir -m 700 "$state"
sha256sum "$base" "$module" "$kernel" "$gpu" "$monitor_config" > "$root/evidence/source-v8-inputs.sha256"
cp --reflink=auto --sparse=always "$base" "$image"
root_mounted=0
resolv_saved=0
resolv_present=0
policy_saved=0
policy_present=0
mirror_saved=0
cleanup() {
    cleanup_failed=0
    if [ "$root_mounted" = 1 ]; then
        if [ "$mirror_saved" = 1 ]; then
            tar -xpf "$state/finalize.apt-sources.tar" -C "$stage/etc/apt" || cleanup_failed=1
            mirror_saved=0
        fi
        if [ "$resolv_saved" = 1 ]; then
            rm -f -- "$stage/etc/resolv.conf"
            if [ "$resolv_present" = 1 ]; then cp -a -- "$state/resolv.conf" "$stage/etc/resolv.conf" || cleanup_failed=1; fi
            resolv_saved=0
        fi
        if [ "$policy_saved" = 1 ]; then
            rm -f -- "$stage/usr/sbin/policy-rc.d"
            if [ "$policy_present" = 1 ]; then cp -a -- "$state/policy-rc.d" "$stage/usr/sbin/policy-rc.d" || cleanup_failed=1; fi
            policy_saved=0
        fi
        for sub in run sys proc dev; do
            if mountpoint -q "$stage/$sub"; then umount -R "$stage/$sub" || cleanup_failed=1; fi
        done
        sync
        if umount "$stage"; then root_mounted=0; else cleanup_failed=1; fi
    fi
    return "$cleanup_failed"
}
trap 'rc=$?; trap - EXIT HUP INT TERM; cleanup || rc=1; exit "$rc"' EXIT
trap 'exit 130' HUP INT TERM
mount -o loop,rw "$image" "$stage"
root_mounted=1
# Compare private account data without printing or persisting its hash.
account_before=$(sha256sum "$stage/etc/shadow")
environment_before=$(sha256sum "$stage/etc/environment")
gdm_before=$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")
recovery="$stage/boot/t736-source-v7-recovery"
test ! -e "$recovery"
mkdir "$recovery"
cp -p "$stage/boot/vmlinuz-$previous" "$stage/boot/initrd.img-$previous" \
    "$stage/boot/config-$previous" "$stage/boot/System.map-$previous" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$recovery/"
(cd "$recovery" && sha256sum vmlinuz-* initrd.img-* config-* System.map-* *.dtb > SHA256SUMS)
for old in v4 v6; do (cd "$stage/boot/t736-source-$old-recovery" && sha256sum -c SHA256SUMS); done
if [ -e "$stage/etc/resolv.conf" ] || [ -L "$stage/etc/resolv.conf" ]; then
    cp -a -- "$stage/etc/resolv.conf" "$state/resolv.conf"; resolv_present=1
fi
resolv_saved=1
rm -f -- "$stage/etc/resolv.conf"
cp -L -- /etc/resolv.conf "$stage/etc/resolv.conf"
if [ -e "$stage/usr/sbin/policy-rc.d" ] || [ -L "$stage/usr/sbin/policy-rc.d" ]; then
    cp -a -- "$stage/usr/sbin/policy-rc.d" "$state/policy-rc.d"; policy_present=1
fi
policy_saved=1
rm -f -- "$stage/usr/sbin/policy-rc.d"
printf '#!/bin/sh\nexit 101\n' > "$stage/usr/sbin/policy-rc.d"
chmod 755 "$stage/usr/sbin/policy-rc.d"
mkdir -p "$stage/dev" "$stage/proc" "$stage/sys" "$stage/run"
mount --rbind /dev "$stage/dev"
mount --make-rslave "$stage/dev"
mount -t proc proc "$stage/proc"
mount -t sysfs -o ro sysfs "$stage/sys"
mount -t tmpfs tmpfs "$stage/run"
mkdir -p "$stage/run/lock"
chroot_run() {
    chroot "$stage" env DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none UCF_FORCE_CONFFOLD=1 "$@"
}
if [ "$(chroot_run dpkg-query -W -f='${Status}' libxcb-dri2-0 2>/dev/null || true)" != 'install ok installed' ]; then
    if [ "$apt_mirror" = aliyun ]; then
        set -- sources.list.d
        if [ -f "$stage/etc/apt/sources.list" ]; then set -- "$@" sources.list; fi
        tar -cpf "$state/finalize.apt-sources.tar" -C "$stage/etc/apt" "$@"
        mirror_saved=1
        find "$stage/etc/apt" -maxdepth 2 -type f \( -name '*.sources' -o -name '*.list' \) -exec sed -i \
            -e 's#https\?://deb.debian.org/debian#https://mirrors.aliyun.com/debian#g' \
            -e 's#https\?://security.debian.org\(/debian-security\)\?/*#https://mirrors.aliyun.com/debian-security/#g' {} +
    fi
    chroot_run apt-get update
    chroot_run apt-get -y --no-install-recommends -o Dpkg::Options::=--force-confdef \
        -o Dpkg::Options::=--force-confold install libxcb-dri2-0
fi
test ! -e "$stage/tmp/t736-v8-kernel.deb" && test ! -e "$stage/tmp/t736-v8-gpu.deb"
cp "$kernel" "$stage/tmp/t736-v8-kernel.deb"
cp "$gpu" "$stage/tmp/t736-v8-gpu.deb"
chroot_run dpkg -i /tmp/t736-v8-kernel.deb /tmp/t736-v8-gpu.deb
rm -f -- "$stage/tmp/t736-v8-kernel.deb" "$stage/tmp/t736-v8-gpu.deb"
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' "linux-image-$release")" = 'install ok installed 6.18.63-15'
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' t736-img-userspace-candidate)" = 'install ok installed 24.2.6603887-1'
grep -qx 'CONFIG_CMA_SIZE_MBYTES=256' "$stage/boot/config-$release"
mkdir -p "$stage/usr/lib/modules/$release/extra" "$stage/usr/lib/firmware"
test ! -e "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
install -m 644 "$module" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
for firmware in rgx.fw.36.56.104.183 rgx.sh.36.56.104.183; do
    # Existing differing firmware is never silently overwritten.
    if [ -e "$stage/usr/lib/firmware/$firmware" ]; then
        cmp "$stage$gpu_prefix/firmware/$firmware" "$stage/usr/lib/firmware/$firmware"
    else
        install -m 644 "$stage$gpu_prefix/firmware/$firmware" "$stage/usr/lib/firmware/$firmware"
    fi
done
chroot_run depmod -a "$release"
chroot_run update-initramfs -u -k "$release"
test "$(chroot_run modinfo -k "$release" -F name pvrsrvkm)" = pvrsrvkm
test "$(chroot_run modinfo -k "$release" -F vermagic pvrsrvkm)" = "$(modinfo -F vermagic "$module")"
# Re-create aliases after Armbian's initramfs hook has finished.
for alias in vmlinuz initrd.img; do
    test ! -e "$stage/boot/$alias-$compat" || test -L "$stage/boot/$alias-$compat"
    ln -sfn "$alias-$release" "$stage/boot/$alias-$compat"
done
cp "$stage/usr/lib/linux-image-$release/allwinner/t736-gpu-candidate.dtb" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
gzip -dc "$stage/boot/vmlinuz-$release" | cmp - "$root/build/linux-gpu-v15/arch/arm64/boot/Image"
cmp "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" \
    "$root/build/linux-gpu-v15/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb"
for directory in /home/t736/.config /var/lib/gdm3/.config /etc/xdg; do
    mkdir -p "$stage$directory"
    install -m 644 "$monitor_config" "$stage$directory/monitors.xml"
done
chroot_run chown t736:t736 /home/t736/.config /home/t736/.config/monitors.xml
chroot_run chown Debian-gdm:Debian-gdm /var/lib/gdm3/.config /var/lib/gdm3/.config/monitors.xml
chown 0:0 "$stage/etc/xdg/monitors.xml"
chroot_run sha256sum -c "$gpu_prefix/SHA256SUMS" > "$root/evidence/source-v8-gpu-payload-check.log"
chroot_run lsinitramfs "/boot/initrd.img-$release" > "$root/output/initrd-source-v8.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$root/output/initrd-source-v8.contents"
test "$account_before" = "$(sha256sum "$stage/etc/shadow")"
test "$environment_before" = "$(sha256sum "$stage/etc/environment")"
test "$gdm_before" = "$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")"
unset account_before environment_before gdm_before
test "$(chroot_run systemctl get-default)" = graphical.target
case "$(chroot_run readlink -f /etc/systemd/system/display-manager.service)" in */gdm.service|*/gdm3.service) ;; *) exit 1 ;; esac
chroot_run apt-get check
chroot_run dpkg --audit > "$state/finalize.dpkg-audit"
test ! -s "$state/finalize.dpkg-audit"
for old in v4 v6 v7; do (cd "$stage/boot/t736-source-$old-recovery" && sha256sum -c SHA256SUMS); done
test ! -L "$stage/etc/fake-hwclock.data"
LC_ALL=C date -u '+%Y-%m-%d %H:%M:%S' > "$stage/etc/fake-hwclock.data"
cp -p "$stage/etc/fake-hwclock.data" "$root/evidence/source-v8-build-clock-utc.txt"
(cd "$stage/boot" && sha256sum "vmlinuz-$release" "initrd.img-$release" dtb/allwinner/t736-mainline-bringup.dtb) \
    > "$root/evidence/source-v8-boot-file-hashes.sha256"
chroot_run dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' > "$root/output/source-v8-dpkg-finalize.tsv"
chroot_run apt-get clean
df -B1 "$stage" > "$root/evidence/source-v8-finalize-space.txt"
sync
cleanup
trap - EXIT HUP INT TERM
e2fsck -fn "$image" > "$root/evidence/source-v8-finalize-fsck.log" 2>&1 || { cat "$root/evidence/source-v8-finalize-fsck.log" >&2; exit 1; }
cat "$root/evidence/source-v8-finalize-fsck.log"
sha256sum "$image" > "$image.sha256"
printf 'finalize complete\n' > "$state/finalize.complete"
printf 'SOURCE_V8_FINALIZE_READY\n'
