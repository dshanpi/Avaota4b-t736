#!/bin/sh
# source-v9/v10 GPU image; distinct from historical build_raw_rootfs_v9.sh.
# Offline only. Never overwrite source-v8 or start a service in the image.
# Usage: sudo sh tools/build_source_rootfs_v9.sh [--variant v9|v10] [--userspace-deb /abs/img.deb] /abs/kernel.deb /abs/pvrsrvkm.ko [software|gpu] [/abs/acceptance.json]
set -eu
variant=v9
userspace=
validation_module=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --variant) [ "$#" -ge 2 ] || exit 2; variant=$2; shift 2 ;;
        --userspace-deb) [ "$#" -ge 2 ] || exit 2; userspace=$2; shift 2 ;;
        --validation-module) [ "$#" -ge 2 ] || exit 2; validation_module=$2; shift 2 ;;
        *) break ;;
    esac
done
case "$variant" in v9|v10) ;; *) printf 'Unsupported source variant: %s\n' "$variant" >&2; exit 2 ;; esac
[ "$#" -ge 2 ] && [ "$#" -le 4 ] || { printf 'Usage: %s [--variant v9|v10] [--userspace-deb /abs/img.deb] /abs/kernel.deb /abs/pvrsrvkm.ko [software|gpu] [/abs/acceptance.json]\n' "$0" >&2; exit 2; }
[ "$(id -u)" = 0 ]
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
case "$1:$2" in /*.deb:/*.ko) ;; *) printf 'Explicit absolute kernel/module inputs required\n' >&2; exit 2 ;; esac
profile=${3:-software}
case "$profile" in software) [ "$#" -le 3 ] ;; gpu) [ "$#" -eq 4 ]; case "$4" in /*) ;; *) exit 2 ;; esac ;; *) exit 2 ;; esac
if [ "$profile" = gpu ] && [ -z "$userspace" ]; then
    printf 'GPU profile requires explicit --userspace-deb with runtime-gles package -2\n' >&2
    exit 2
fi
if [ "${T736_V9_IN_NAMESPACE:-0}" != 1 ]; then
    if [ -n "$userspace" ]; then
        set -- --userspace-deb "$userspace" "$@"
    fi
    if [ -n "$validation_module" ]; then
        set -- --validation-module "$validation_module" "$@"
    fi
    exec unshare --mount --propagation private env T736_V9_IN_NAMESPACE=1 \
        T736_V9_CALLER_NS="$(readlink /proc/self/ns/mnt)" sh "$0" --variant "$variant" "$@"
fi
[ -n "${T736_V9_CALLER_NS:-}" ]
[ "$(readlink /proc/self/ns/mnt)" != "$T736_V9_CALLER_NS" ]
mount --make-rprivate /
mkdir -p "$root/build" "$root/output" "$root/evidence"
exec 9> "$root/build/source-rootfs-$variant.lock"
flock -n 9
kernel=$(realpath -e -- "$1")
module=$(realpath -e -- "$2")
release=6.18.63-t736-gpu-v15
base="$root/output/rootfs-armbian-source-v8.ext4"
image="$root/output/rootfs-armbian-source-$variant.ext4"
stage="$root/build/source-rootfs-$variant-mount"
state="$root/build/source-rootfs-$variant-state"
profile_tool="$root/tools/validation/gpu_profile.py"
test -s "$kernel" && test -s "$module" && test -s "$profile_tool"
if [ -n "$validation_module" ]; then
    case "$validation_module" in /*.ko) ;; *) exit 2 ;; esac
    validation_module=$(realpath -e -- "$validation_module")
    case "$(modinfo -F vermagic "$validation_module")" in "$release "*) ;; *) exit 2 ;; esac
fi
test "$(dpkg-deb -f "$kernel" Package)" = "linux-image-$release"
test "$(dpkg-deb -f "$kernel" Architecture)" = arm64
test "$(dpkg-deb -f "$kernel" Version)" = 6.18.63-15
userspace_version=24.2.6603887-1
if [ -n "$userspace" ]; then
    case "$userspace" in /*.deb) ;; *) exit 2 ;; esac
    userspace=$(realpath -e -- "$userspace")
    test "$(dpkg-deb -f "$userspace" Package)" = t736-img-userspace-candidate
    test "$(dpkg-deb -f "$userspace" Architecture)" = arm64
    userspace_version=24.2.6603887-2
    test "$(dpkg-deb -f "$userspace" Version)" = "$userspace_version"
fi
test "$(modinfo -F name "$module")" = pvrsrvkm
case "$(modinfo -F vermagic "$module")" in "$release "*) ;; *) printf 'Module vermagic mismatch\n' >&2; exit 1 ;; esac
strings -a "$module" | grep -Fx 'Rogue_DDK_Linux_WS rogueddk 24.2@6603887' >/dev/null || { printf 'Module DDK mismatch\n' >&2; exit 1; }
test -f "$root/build/source-rootfs-v8-state/finalize.complete"
sha256sum -c --status "$base.sha256"
test "$(stat -c %s "$base")" = 7802716160
test ! -e "$image" && test ! -L "$image"
test ! -e "$state" && test ! -L "$state"
test ! -L "$stage"
mkdir -p "$stage"
test -z "$(find "$stage" -mindepth 1 -maxdepth 1 -print -quit)"
mkdir -m 700 "$state"
# Freeze the explicit inputs: a later kernel rebuild at the original paths must
# not change what this image installs or what its acceptance record describes.
printf 'kernel=%s\nmodule=%s\nuserspace=%s\n' "$kernel" "$module" "$userspace" > "$state/input-source-paths.txt"
printf 'validation_module=%s\n' "$validation_module" >> "$state/input-source-paths.txt"
kernel_sha=$(sha256sum "$kernel" | cut -d ' ' -f 1)
module_sha=$(sha256sum "$module" | cut -d ' ' -f 1)
cp -- "$kernel" "$state/kernel-input.deb"
cp -- "$module" "$state/pvrsrvkm-input.ko"
kernel="$state/kernel-input.deb"
module="$state/pvrsrvkm-input.ko"
printf '%s  %s\n%s  %s\n' "$kernel_sha" "$kernel" "$module_sha" "$module" | sha256sum -c -
if [ -n "$userspace" ]; then
    userspace_sha=$(sha256sum "$userspace" | cut -d ' ' -f 1)
    cp -- "$userspace" "$state/userspace-input.deb"
    userspace="$state/userspace-input.deb"
    printf '%s  %s\n' "$userspace_sha" "$userspace" | sha256sum -c -
fi
if [ -n "$validation_module" ]; then
    validation_sha=$(sha256sum "$validation_module" | cut -d ' ' -f 1)
    cp -- "$validation_module" "$state/validation-input.ko"
    validation_module="$state/validation-input.ko"
    printf '%s  %s\n' "$validation_sha" "$validation_module" | sha256sum -c -
fi
if [ "$profile" = gpu ]; then
    acceptance=$(realpath -e -- "$4")
    python3 - "$kernel" "$userspace" "$acceptance" <<'PY'
import hashlib, json, pathlib, sys
kernel, userspace, evidence = map(pathlib.Path, sys.argv[1:])
record = json.loads(evidence.read_text())
digest = hashlib.sha256()
with kernel.open('rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(block)
actual = digest.hexdigest()
if record.get('kernel_deb_sha256') != actual:
    raise SystemExit('Acceptance record does not match the supplied kernel package')
if record.get('userspace_deb_sha256') != hashlib.sha256(userspace.read_bytes()).hexdigest():
    raise SystemExit('Acceptance record does not match the supplied userspace package')
PY
    cp -- "$acceptance" "$state/gpu-acceptance.json"
fi
sha256sum "$base" "$kernel" "$module" "$profile_tool" \
    "$root/tools/validation/gpu_runtime_check.sh" > "$state/inputs.sha256"
if [ -n "$userspace" ]; then sha256sum "$userspace" >> "$state/inputs.sha256"; fi
if [ -n "$validation_module" ]; then sha256sum "$validation_module" >> "$state/inputs.sha256"; fi
sha256sum "$root/config/gpu-v10/20-t736-modesetting-1080p.conf" \
    "$root/tools/validation/gpu_session_trial.sh" "$root/tools/validation/mutter_set_1080p.py" >> "$state/inputs.sha256"
cp --reflink=auto --sparse=always "$base" "$image"
mounted=0
policy_saved=0
policy_present=0
cleanup() {
    failed=0
    if [ "$mounted" = 1 ]; then
        if [ "$policy_saved" = 1 ]; then
            rm -f -- "$stage/usr/sbin/policy-rc.d"
            if [ "$policy_present" = 1 ]; then cp -a -- "$state/policy-rc.d" "$stage/usr/sbin/policy-rc.d" || failed=1; fi
            policy_saved=0
        fi
        for sub in run sys proc dev; do
            if mountpoint -q "$stage/$sub"; then umount -R "$stage/$sub" || failed=1; fi
        done
        sync
        if umount "$stage"; then mounted=0; else failed=1; fi
    fi
    return "$failed"
}
trap 'rc=$?; trap - EXIT HUP INT TERM; cleanup || rc=1; exit "$rc"' EXIT
trap 'exit 130' HUP INT TERM
mount -o loop,rw "$image" "$stage"
mounted=1
account_before=$(sha256sum "$stage/etc/shadow")
gdm_before=$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")
test ! -e "$stage/root/t736-gpu-session-trial/active"
test ! -e "$stage/var/lib/t736-gpu-profile"
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
chroot_run() { chroot "$stage" env DEBIAN_FRONTEND=noninteractive "$@"; }
test ! -e "$stage/tmp/t736-v9-kernel.deb"
cp -- "$kernel" "$stage/tmp/t736-v9-kernel.deb"
if [ -n "$userspace" ]; then
    test ! -e "$stage/tmp/t736-v9-userspace.deb"
    cp -- "$userspace" "$stage/tmp/t736-v9-userspace.deb"
    chroot_run dpkg -i /tmp/t736-v9-kernel.deb /tmp/t736-v9-userspace.deb
    rm -f -- "$stage/tmp/t736-v9-userspace.deb"
else
    chroot_run dpkg -i /tmp/t736-v9-kernel.deb
fi
rm -f -- "$stage/tmp/t736-v9-kernel.deb"
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' "linux-image-$release")" = 'install ok installed 6.18.63-15'
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' t736-img-userspace-candidate)" = "install ok installed $userspace_version"
grep -qx 'CONFIG_CMA_SIZE_MBYTES=256' "$stage/boot/config-$release"
mkdir -p "$stage/usr/lib/modules/$release/extra"
install -m 644 "$module" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
cmp "$module" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
chroot_run depmod -a "$release"
chroot_run update-initramfs -u -k "$release"
test "$(chroot_run modinfo -k "$release" -F vermagic pvrsrvkm)" = "$(modinfo -F vermagic "$module")"
for alias in vmlinuz initrd.img; do
    test ! -e "$stage/boot/$alias-6.18.63-t736-armbian" || test -L "$stage/boot/$alias-6.18.63-t736-armbian"
    ln -sfn "$alias-$release" "$stage/boot/$alias-6.18.63-t736-armbian"
done
cp "$stage/usr/lib/linux-image-$release/allwinner/t736-gpu-candidate.dtb" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
chroot_run sha256sum -c /opt/t736-img-ddk-24.2-x11/SHA256SUMS > "$state/gpu-payload-check.log"
if [ -n "$userspace" ]; then
    python3 "$stage/opt/t736-img-ddk-24.2-x11/bin/gpu-runtime-view" --root "$stage" --verify > "$state/gpu-runtime-view-check.json"
fi
mkdir -p "$stage/usr/local/sbin" "$stage/usr/share/doc/t736-gpu-profile"
mkdir -p "$stage/usr/share/t736-gpu-profile" "$stage/usr/local/lib/t736-validation"
install -m 644 "$root/config/gpu-v10/20-t736-modesetting-1080p.conf" "$stage/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf"
install -m 755 "$profile_tool" "$stage/usr/local/sbin/t736-gpu-profile"
install -m 755 "$root/tools/validation/gpu_runtime_check.sh" "$stage/usr/local/sbin/t736-gpu-runtime-check"
install -m 755 "$root/tools/validation/gpu_session_trial.sh" "$stage/usr/local/sbin/t736-gpu-session-trial"
install -m 755 "$root/tools/validation/mutter_set_1080p.py" "$stage/usr/local/sbin/t736-mutter-1080p"
if [ -n "$validation_module" ]; then
    install -m 644 "$validation_module" "$stage/usr/local/lib/t736-validation/t736-a76-clock-trial.ko"
fi
# Match the serial-console diagnostic baseline without suppressing dmesg logs.
printf 'kernel.printk = 4\n' > "$stage/etc/sysctl.d/90-t736-console.conf"
install -m 644 "$root/tools/validation/gpu_profile.README.md" "$stage/usr/share/doc/t736-gpu-profile/README.md"
# Host Python with --root only edits image files. It never invokes systemctl.
python3 "$profile_tool" software --root "$stage"
if [ "$profile" = gpu ]; then
    python3 "$profile_tool" gpu --root "$stage" --evidence "$state/gpu-acceptance.json"
fi
python3 - "$stage" "$kernel" "$module" "$profile" "$state" "$variant" "$userspace" <<'PY'
import hashlib, json, pathlib, sys
stage, kernel, module, profile, state, variant = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]), sys.argv[4], pathlib.Path(sys.argv[5]), sys.argv[6]
def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()
paths = ['/boot/vmlinuz-6.18.63-t736-gpu-v15', '/boot/initrd.img-6.18.63-t736-gpu-v15',
         '/boot/config-6.18.63-t736-gpu-v15', '/boot/dtb/allwinner/t736-mainline-bringup.dtb',
         '/usr/lib/modules/6.18.63-t736-gpu-v15/extra/pvrsrvkm.ko', '/etc/environment',
         '/etc/X11/xorg.conf.d/20-t736-modesetting.conf',
         '/etc/systemd/system/gdm.service.d/90-t736-gpu-profile.conf', '/etc/modules-load.d/t736-gpu.conf']
paths += ['/opt/t736-img-ddk-24.2-x11/manifest.json', '/opt/t736-img-ddk-24.2-x11/runtime-gles.json']
paths += ['/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf',
          '/usr/local/sbin/t736-gpu-profile', '/usr/local/sbin/t736-gpu-session-trial',
          '/usr/local/sbin/t736-mutter-1080p', '/usr/local/lib/t736-validation/t736-a76-clock-trial.ko',
          '/etc/sysctl.d/90-t736-console.conf']
record = {'schema': 1, 'variant': variant, 'desktop_profile': profile, 'kernel_release': '6.18.63-t736-gpu-v15',
          'kernel_deb_sha256': sha(kernel), 'module_input_sha256': sha(module),
          'userspace_deb_sha256': sha(pathlib.Path(sys.argv[7])) if sys.argv[7] else None,
          'installed_files': {p: sha(stage / p.lstrip('/')) for p in paths if (stage / p.lstrip('/')).is_file()},
          'final_image_cold_boot_validated': False,
          'gpu_acceptance_sha256': sha(state / 'gpu-acceptance.json') if profile == 'gpu' else None}
(state / 'profile-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
(stage / 'usr/share/doc/t736-gpu-profile/build-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
PY
test "$account_before" = "$(sha256sum "$stage/etc/shadow")"
test "$gdm_before" = "$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")"
unset account_before gdm_before
for old in v4 v6 v7; do (cd "$stage/boot/t736-source-$old-recovery" && sha256sum -c SHA256SUMS); done
chroot_run lsinitramfs "/boot/initrd.img-$release" > "$state/initrd.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$state/initrd.contents"
test "$(chroot_run systemctl get-default)" = graphical.target
chroot_run dpkg --audit > "$state/dpkg-audit"
test ! -s "$state/dpkg-audit"
chroot_run apt-get check
test ! -L "$stage/etc/fake-hwclock.data"
LC_ALL=C date -u '+%Y-%m-%d %H:%M:%S' > "$stage/etc/fake-hwclock.data"
chroot_run dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' > "$state/dpkg-final.tsv"
sync
cleanup
trap - EXIT HUP INT TERM
e2fsck -fn "$image" > "$state/finalize-fsck.log" 2>&1 || { cat "$state/finalize-fsck.log" >&2; exit 1; }
sha256sum "$image" > "$image.sha256"
printf 'finalize complete\n' > "$state/finalize.complete"
printf 'SOURCE_ROOTFS_FINALIZE_READY variant=%s profile=%s\n' "$variant" "$profile"
