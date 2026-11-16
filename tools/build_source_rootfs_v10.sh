#!/bin/sh
# Draft source-v10: storage-v16, preserved v15 recovery, GLES1080 and FFmpeg.
# Default --check never mounts/copies an image. --build is explicit and has not been run.
# CPU-D and Firefox acceptance are separate records; this draft never creates them.
# Offline only. Never overwrite source-v9 or start a service in the image.
# Usage: sh tools/build_source_rootfs_v10.sh --check --ffmpeg-version VERSION --libavcodec-version VERSION --userspace-deb /abs/img-2.deb /abs/kernel-v16.deb /abs/pvrsrvkm-v16.ko gpu /abs/v16-gpu-acceptance.json
set -eu
mode=check
case "${1:-}" in --check) shift ;; --build) mode=build; shift ;; esac
ffmpeg_version=
libavcodec_version=
variant=v10
userspace=
validation_module=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --ffmpeg-version) [ "$#" -ge 2 ] || exit 2; ffmpeg_version=$2; shift 2 ;;
        --libavcodec-version) [ "$#" -ge 2 ] || exit 2; libavcodec_version=$2; shift 2 ;;
        --variant) [ "$#" -ge 2 ] || exit 2; variant=$2; shift 2 ;;
        --userspace-deb) [ "$#" -ge 2 ] || exit 2; userspace=$2; shift 2 ;;
        --validation-module) [ "$#" -ge 2 ] || exit 2; validation_module=$2; shift 2 ;;
        *) break ;;
    esac
done
case "$variant" in v10) ;; *) printf 'Unsupported source variant: %s\n' "$variant" >&2; exit 2 ;; esac
[ "$#" -eq 4 ] || { printf 'Usage: %s [--check|--build] --ffmpeg-version VERSION --libavcodec-version VERSION --userspace-deb /abs/img-2.deb /abs/kernel-v16.deb /abs/pvrsrvkm-v16.ko gpu /abs/v16-gpu-acceptance.json\n' "$0" >&2; exit 2; }
[ "$(id -u)" = 0 ]
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
[ -n "$validation_module" ] || validation_module="$root/output/t736-cpu-clock-readonly-storage-v16.ko"
monitor_input="$root/config/gpu-v10/user-monitors.xml"
printf '5d72649064afb2a9ac9866c85b2875d0e85f1c98a1126ce1379c9236d4f995f8  %s\n' "$monitor_input" | sha256sum -c -
case "$1:$2" in /*.deb:/*.ko) ;; *) printf 'Explicit absolute kernel/module inputs required\n' >&2; exit 2 ;; esac
profile=${3:-gpu}
case "$profile" in software) [ "$#" -le 3 ] ;; gpu) [ "$#" -eq 4 ]; case "$4" in /*) ;; *) exit 2 ;; esac ;; *) exit 2 ;; esac
if [ "$profile" = gpu ] && [ -z "$userspace" ]; then
    printf 'GPU profile requires explicit --userspace-deb with runtime-gles package -2\n' >&2
    exit 2
fi
# Require explicit final package versions; do not guess from stale apt metadata.
[ "$profile" = gpu ] && [ "$#" -eq 4 ] && [ -n "$userspace" ]
[ -n "$ffmpeg_version" ] && [ -n "$libavcodec_version" ]
case "$ffmpeg_version:$libavcodec_version" in *[!A-Za-z0-9.+:~_-]*) exit 2 ;; esac
# Validate the exact reviewed package/module pair before creating any state or
# image. The imported function performs archive/modinfo checks only, no device I/O.
python3 - "$root" "$1" "$2" "$userspace" "$4" <<'PYPREFLIGHT'
import hashlib, json, pathlib, sys
root, kernel, module, userspace, evidence = map(pathlib.Path, sys.argv[1:])
sys.path.insert(0, str(root / 'tools'))
from validation.update_t736_storage_v16 import inputs
inputs(kernel, module)
assert hashlib.sha256(userspace.read_bytes()).hexdigest() == '1455889d0c10ec5d0b49e839e0e9d3f7560869e65da5fbee02e477039c78e16e', 'Userspace -2 package changed'
record = json.loads(evidence.read_text())
assert record.get('schema') == 1 and record.get('board') == 't736'
assert record.get('status') == 'passed' and record.get('kernel_release') == '6.18.63-t736-storage-v16', 'A real v16 GPU acceptance record is required'
assert record.get('kernel_deb_sha256') == hashlib.sha256(kernel.read_bytes()).hexdigest()
assert record.get('userspace_deb_sha256') == hashlib.sha256(userspace.read_bytes()).hexdigest()
assert all(record.get('checks', {}).get(name) is True for name in ('gbm_probe', 'x11_probe', 'gnome_hardware_renderer', 'gtk_hardware_renderer', 'hdmi_1080p'))
assert record.get('evidence_logs'), 'Raw evidence logs are required'
for name, expected in record['evidence_logs'].items():
    relative = pathlib.Path(name)
    assert not relative.is_absolute() and '..' not in relative.parts, 'Invalid evidence path'
    path = (root / ('evidence' if len(relative.parts) == 1 else '') / relative).resolve(strict=True)
    assert path.is_relative_to(root) and path.is_file(), 'Evidence path escapes project'
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, 'Evidence log changed: ' + name
PYPREFLIGHT
if [ "$mode" = check ]; then
    test "$(dpkg-deb -f "$1" Package)" = linux-image-6.18.63-t736-storage-v16
    test "$(dpkg-deb -f "$1" Version)" = 6.18.63-16
    test "$(dpkg-deb -f "$1" Architecture)" = arm64
    test "$(modinfo -F name "$2")" = pvrsrvkm
    case "$(modinfo -F vermagic "$2")" in '6.18.63-t736-storage-v16 '*) ;; *) exit 2 ;; esac
    test "$(dpkg-deb -f "$userspace" Package)" = t736-img-userspace-candidate
    test "$(dpkg-deb -f "$userspace" Version)" = 24.2.6603887-2
    test "$(dpkg-deb -f "$userspace" Architecture)" = arm64
    test -s "$4"
    printf 'CHECK_INPUTS_ONLY: storage-v16 ABI/package checked; no image/mount/install/write performed.\n'
    printf 'CPU-D/Firefox/new GPU acceptance must be reviewed separately; this is not hardware acceptance.\n'
    exit 0
fi
[ "$(id -u)" = 0 ]
if [ "${T736_V10_IN_NAMESPACE:-0}" != 1 ]; then
    if [ -n "$userspace" ]; then
        set -- --userspace-deb "$userspace" "$@"
    fi
    if [ -n "$validation_module" ]; then
        set -- --validation-module "$validation_module" "$@"
    fi
    exec unshare --mount --propagation private env T736_V10_IN_NAMESPACE=1 \
        T736_V10_CALLER_NS="$(readlink /proc/self/ns/mnt)" sh "$0" --build --ffmpeg-version "$ffmpeg_version" --libavcodec-version "$libavcodec_version" --variant "$variant" "$@"
fi
[ -n "${T736_V10_CALLER_NS:-}" ]
[ "$(readlink /proc/self/ns/mnt)" != "$T736_V10_CALLER_NS" ]
mount --make-rprivate /
mkdir -p "$root/build" "$root/output" "$root/evidence"
exec 9> "$root/build/source-rootfs-$variant.lock"
flock -n 9
kernel=$(realpath -e -- "$1")
module=$(realpath -e -- "$2")
release=6.18.63-t736-storage-v16
base="$root/output/rootfs-armbian-source-v9.ext4"
image="$root/output/rootfs-armbian-source-$variant.ext4"
stage="$root/build/source-rootfs-$variant-mount"
state="$root/build/source-rootfs-$variant-state"
profile_tool="$root/tools/validation/gpu_profile.py"
test -s "$kernel" && test -s "$module" && test -s "$profile_tool"
if [ -n "$validation_module" ]; then
    case "$validation_module" in /*.ko) ;; *) exit 2 ;; esac
    validation_module=$(realpath -e -- "$validation_module")
    case "$(modinfo -F vermagic "$validation_module")" in "$release "*) ;; *) exit 2 ;; esac
    test "$(modinfo -F name "$validation_module")" = t736_cpu_clock_readonly
    printf '175f47bdf40cc42dae47a01e0c375b04d83acf802946539005d0a51ba3ddaf91  %s\n' "$validation_module" | sha256sum -c -
fi
test "$(dpkg-deb -f "$kernel" Package)" = "linux-image-$release"
test "$(dpkg-deb -f "$kernel" Architecture)" = arm64
test "$(dpkg-deb -f "$kernel" Version)" = 6.18.63-16
userspace_version=24.2.6603887-2
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
test -f "$root/build/source-rootfs-v9-state/finalize.complete"
sha256sum -c --status "$base.sha256"
test "$(stat -c %s "$base")" = 7802716160
test ! -e "$image" && test ! -L "$image"
test ! -e "$state" && test ! -L "$state"
test ! -L "$stage"
mkdir -p "$stage"
test -z "$(find "$stage" -mindepth 1 -maxdepth 1 -print -quit)"
mkdir -m 700 "$state"
# Bind only the release guards, preserving the reviewed v15 sources and hashes.
python3 "$root/tools/validation/prepare_gpu_storage_v16.py" --root "$root" --output "$state/gpu-helpers" > "$state/gpu-helper-generation.json"
profile_tool="$state/gpu-helpers/t736-gpu-profile"
runtime_tool="$state/gpu-helpers/t736-gpu-runtime-check"
cp -- "$monitor_input" "$state/user-monitors.xml"
printf '5d72649064afb2a9ac9866c85b2875d0e85f1c98a1126ce1379c9236d4f995f8  %s\n' "$state/user-monitors.xml" | sha256sum -c -
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
sha256sum "$base" "$kernel" "$module" "$profile_tool" "$runtime_tool" \
    "$state/gpu-helpers/manifest.json" "$state/user-monitors.xml" > "$state/inputs.sha256"
if [ -n "$userspace" ]; then sha256sum "$userspace" >> "$state/inputs.sha256"; fi
if [ -n "$validation_module" ]; then sha256sum "$validation_module" >> "$state/inputs.sha256"; fi
sha256sum "$state/gpu-helpers/20-t736-modesetting-1080p.conf" \
    "$state/gpu-helpers/t736-mutter-1080p" >> "$state/inputs.sha256"
cp --reflink=auto --sparse=always "$base" "$image"
mounted=0
policy_saved=0
policy_present=0
resolv_saved=0
hook_mounts=
cleanup() {
    failed=0
    if [ "$mounted" = 1 ]; then
        for hook in $hook_mounts; do umount "$stage$hook" || failed=1; done
        hook_mounts=
        if [ "$resolv_saved" = 1 ]; then
            rm -f -- "$stage/etc/resolv.conf"
            cp -a -- "$state/resolv.conf.before" "$stage/etc/resolv.conf" || failed=1
            resolv_saved=0
        fi
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
previous=6.18.63-t736-gpu-v15
recovery="$stage/boot/t736-source-v15-recovery"
test ! -e "$recovery"
mkdir -m 700 "$recovery"
cp -p "$stage/boot/vmlinuz-$previous" "$stage/boot/initrd.img-$previous" \
    "$stage/boot/config-$previous" "$stage/boot/System.map-$previous" \
    "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$recovery/"
(cd "$recovery" && sha256sum vmlinuz-* initrd.img-* config-* System.map-* *.dtb > SHA256SUMS)
# Old trial tools contain v15/v7 recovery assumptions. Retain them as explicit
# legacy recovery artifacts, never as operational v16 helpers/modules.
mkdir -m 700 "$recovery/legacy-tools"
for legacy in /usr/local/sbin/t736-gpu-profile /usr/local/sbin/t736-gpu-runtime-check \
    /usr/local/sbin/t736-gpu-session-trial /usr/local/lib/t736-validation/t736-a76-clock-trial.ko; do
    if [ -e "$stage$legacy" ]; then
        test -f "$stage$legacy" && test ! -L "$stage$legacy"
        mv -- "$stage$legacy" "$recovery/legacy-tools/"
    fi
done
(cd "$recovery/legacy-tools" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 -r sha256sum > SHA256SUMS)
(cd "$stage/usr/lib/modules/$previous" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$state/v15-modules.sha256"
for alias in vmlinuz initrd.img; do
    test "$(readlink "$stage/boot/$alias-6.18.63-t736-armbian")" = "$alias-$previous"
done
cp -a -- "$stage/etc/resolv.conf" "$state/resolv.conf.before"
resolv_saved=1
rm -f -- "$stage/etc/resolv.conf"
cp -L -- /etc/resolv.conf "$stage/etc/resolv.conf"
# Suppress package/post-update boot selection hooks only inside this private
# mount namespace. The explicit final checks and switch below own publication.
mkdir -m 700 "$state/empty-boot-hooks"
for hook in /etc/kernel /usr/share/kernel /etc/initramfs/post-update.d; do
    if [ -d "$stage$hook" ]; then
        mount --bind "$state/empty-boot-hooks" "$stage$hook"
        hook_mounts="$hook $hook_mounts"
    fi
done
account_before=$(sha256sum "$stage/etc/shadow")
gdm_before=$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")
test ! -e "$stage/root/t736-gpu-session-trial/active"
test ! -e "$stage/var/lib/t736-gpu-profile/pending.json"
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
test ! -e "$stage/tmp/t736-v10-kernel.deb"
cp -- "$kernel" "$stage/tmp/t736-v10-kernel.deb"
if [ -n "$userspace" ]; then
    test ! -e "$stage/tmp/t736-v10-userspace.deb"
    cp -- "$userspace" "$stage/tmp/t736-v10-userspace.deb"
    chroot_run dpkg -i /tmp/t736-v10-kernel.deb /tmp/t736-v10-userspace.deb
    rm -f -- "$stage/tmp/t736-v10-userspace.deb"
else
    chroot_run dpkg -i /tmp/t736-v10-kernel.deb
fi
rm -f -- "$stage/tmp/t736-v10-kernel.deb"
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' "linux-image-$release")" = 'install ok installed 6.18.63-16'
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' t736-img-userspace-candidate)" = "install ok installed $userspace_version"
grep -qx 'CONFIG_CMA_SIZE_MBYTES=256' "$stage/boot/config-$release"
mkdir -p "$stage/usr/lib/modules/$release/extra"
install -m 644 "$module" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
cmp "$module" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
chroot_run apt-get update
# An existing v9 base can predate the desktop package-list correction. Install
# the GL elements explicitly here; libgstgl alone does not provide glsinkbin.
chroot_run apt-get install -y --no-install-recommends "ffmpeg=$ffmpeg_version" "libavcodec61=$libavcodec_version" gstreamer1.0-gl
test "$(chroot_run dpkg-query -W -f='${Version}' ffmpeg)" = "$ffmpeg_version"
test "$(chroot_run dpkg-query -W -f='${Version}' libavcodec61)" = "$libavcodec_version"
test "$(chroot_run dpkg-query -W -f='${Status}' gstreamer1.0-gl)" = 'install ok installed'
gst_gl_version=$(chroot_run dpkg-query -W -f='${Version}' gstreamer1.0-gl)
test -s "$stage/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstopengl.so"
chroot_run depmod -a "$release"
if [ -s "$stage/boot/initrd.img-$release" ]; then
    chroot_run update-initramfs -u -k "$release"
else
    chroot_run update-initramfs -c -k "$release"
fi
test "$(chroot_run modinfo -k "$release" -F vermagic pvrsrvkm)" = "$(modinfo -F vermagic "$module")"
# Validate final bytes before touching any fixed v9 boot alias or DTB.
gzip -dc "$stage/boot/vmlinuz-$release" | cmp - "$root/build/linux-storage-v16/arch/arm64/boot/Image"
cmp "$stage/boot/config-$release" "$root/build/linux-storage-v16/.config"
cmp "$stage/boot/System.map-$release" "$root/build/linux-storage-v16/System.map"
cmp "$stage/usr/lib/linux-image-$release/allwinner/t736-gpu-candidate.dtb" \
    "$root/build/linux-storage-v16/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb"
chroot_run lsinitramfs "/boot/initrd.img-$release" > "$state/initrd.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$state/initrd.contents"
grep -F "lib/modules/$release/" "$state/initrd.contents" >/dev/null
(cd "$recovery" && sha256sum -c SHA256SUMS)
for oldfile in vmlinuz initrd.img config System.map; do cmp "$stage/boot/$oldfile-$previous" "$recovery/$oldfile-$previous"; done
(cd "$stage/usr/lib/modules/$previous" && sha256sum -c "$state/v15-modules.sha256")
for alias in vmlinuz initrd.img; do
    test "$(readlink "$stage/boot/$alias-6.18.63-t736-armbian")" = "$alias-$previous"
done
sha256sum "$stage/boot/vmlinuz-$release" "$stage/boot/initrd.img-$release" \
    "$stage/usr/lib/linux-image-$release/allwinner/t736-gpu-candidate.dtb" \
    "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko" > "$state/pre-switch.sha256"
sha256sum -c "$state/pre-switch.sha256"
# These aliases keep the existing v9 U-Boot bootcmd unchanged.
python3 - "$stage" "$release" <<'PYSWITCH'
from pathlib import Path
import os, sys
root, release = Path(sys.argv[1]), sys.argv[2]
for name in ('vmlinuz', 'initrd.img'):
    target = root/'boot'/(name+'-6.18.63-t736-armbian')
    temporary = target.with_name(target.name+'.storage-v16-new')
    assert not temporary.exists() and not temporary.is_symlink()
    temporary.symlink_to(name+'-'+release)
    os.replace(temporary, target)
dtb = root/'boot/dtb/allwinner/t736-mainline-bringup.dtb'
data = (root/'usr/lib'/('linux-image-'+release)/'allwinner/t736-gpu-candidate.dtb').read_bytes()
temporary = dtb.with_name(dtb.name+'.storage-v16-new')
with temporary.open('xb') as output:
    output.write(data); output.flush(); os.fsync(output.fileno())
os.replace(temporary, dtb)
for directory in (root/'boot', dtb.parent):
    fd=os.open(directory, os.O_RDONLY|os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)
PYSWITCH
sha256sum -c "$state/pre-switch.sha256"
chroot_run sha256sum -c /opt/t736-img-ddk-24.2-x11/SHA256SUMS > "$state/gpu-payload-check.log"
if [ -n "$userspace" ]; then
    python3 "$stage/opt/t736-img-ddk-24.2-x11/bin/gpu-runtime-view" --root "$stage" --verify > "$state/gpu-runtime-view-check.json"
fi
mkdir -p "$stage/usr/local/sbin" "$stage/usr/share/doc/t736-gpu-profile"
mkdir -p "$stage/usr/share/t736-gpu-profile" "$stage/usr/local/lib/t736-validation"
install -m 644 "$state/gpu-helpers/20-t736-modesetting-1080p.conf" "$stage/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf"
install -m 755 "$profile_tool" "$stage/usr/local/sbin/t736-gpu-profile"
install -m 755 "$runtime_tool" "$stage/usr/local/sbin/t736-gpu-runtime-check"
install -m 755 "$state/gpu-helpers/t736-mutter-1080p" "$stage/usr/local/sbin/t736-mutter-1080p"
# Preserve the tested physical 1080p layout for both login and user sessions;
# account paths/ownership come from the copied rootfs, not host passwd entries.
for account in t736 Debian-gdm; do
    entry=$(chroot_run getent passwd "$account")
    account_uid=$(printf '%s\n' "$entry" | cut -d: -f3)
    account_gid=$(printf '%s\n' "$entry" | cut -d: -f4)
    account_home=$(printf '%s\n' "$entry" | cut -d: -f6)
    case "$account:$account_home" in t736:/home/t736|Debian-gdm:/var/lib/gdm3) ;; *) printf 'Unexpected desktop account home\n' >&2; exit 2 ;; esac
    test -d "$stage$account_home" && test ! -L "$stage$account_home/.config"
    if [ ! -d "$stage$account_home/.config" ]; then install -d -m 700 -o "$account_uid" -g "$account_gid" "$stage$account_home/.config"; fi
    install -m 644 -o "$account_uid" -g "$account_gid" "$state/user-monitors.xml" "$stage$account_home/.config/monitors.xml"
done
if [ -n "$validation_module" ]; then
    install -m 644 "$validation_module" "$stage/usr/local/lib/t736-validation/t736-cpu-clock-readonly-storage-v16.ko"
fi
# Match the serial-console diagnostic baseline without suppressing dmesg logs.
printf 'kernel.printk = 4\n' > "$stage/etc/sysctl.d/90-t736-console.conf"
install -m 644 "$root/tools/validation/gpu_profile.README.md" "$stage/usr/share/doc/t736-gpu-profile/README.md"
# Host Python with --root only edits image files. It never invokes systemctl.
python3 "$profile_tool" software --root "$stage"
if [ "$profile" = gpu ]; then
    python3 "$profile_tool" gpu --root "$stage" --evidence "$state/gpu-acceptance.json"
fi
python3 - "$stage" "$kernel" "$module" "$profile" "$state" "$variant" "$userspace" "$ffmpeg_version" "$libavcodec_version" "$gst_gl_version" <<'PY'
import hashlib, json, pathlib, sys
stage, kernel, module, profile, state, variant = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]), sys.argv[4], pathlib.Path(sys.argv[5]), sys.argv[6]
def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()
paths = ['/boot/vmlinuz-6.18.63-t736-storage-v16', '/boot/initrd.img-6.18.63-t736-storage-v16',
         '/boot/config-6.18.63-t736-storage-v16', '/boot/dtb/allwinner/t736-mainline-bringup.dtb',
         '/usr/lib/modules/6.18.63-t736-storage-v16/extra/pvrsrvkm.ko', '/etc/environment',
         '/etc/X11/xorg.conf.d/20-t736-modesetting.conf',
         '/etc/systemd/system/gdm.service.d/90-t736-gpu-profile.conf', '/etc/modules-load.d/t736-gpu.conf']
paths += ['/opt/t736-img-ddk-24.2-x11/manifest.json', '/opt/t736-img-ddk-24.2-x11/runtime-gles.json']
paths += ['/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstopengl.so']
paths += ['/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf',
          '/usr/local/sbin/t736-gpu-profile', '/usr/local/sbin/t736-gpu-runtime-check',
          '/usr/local/sbin/t736-mutter-1080p', '/usr/local/lib/t736-validation/t736-cpu-clock-readonly-storage-v16.ko',
          '/home/t736/.config/monitors.xml', '/var/lib/gdm3/.config/monitors.xml',
          '/etc/sysctl.d/90-t736-console.conf']
record = {'schema': 1, 'variant': variant, 'desktop_profile': profile, 'kernel_release': '6.18.63-t736-storage-v16',
          'kernel_deb_sha256': sha(kernel), 'module_input_sha256': sha(module),
          'userspace_deb_sha256': sha(pathlib.Path(sys.argv[7])) if sys.argv[7] else None,
          'installed_files': {p: sha(stage / p.lstrip('/')) for p in paths if (stage / p.lstrip('/')).is_file()},
          'final_image_cold_boot_validated': False,
          'cpu_d_cold_boot_validated': False, 'firefox_renderer_validated': False,
          'ffmpeg_installed': True, 'previous_kernel_retained': '6.18.63-t736-gpu-v15',
          'media_packages': {'ffmpeg': sys.argv[8], 'libavcodec61': sys.argv[9], 'gstreamer1.0-gl': sys.argv[10]},
          'gpu_helper_manifest': json.loads((state / 'gpu-helpers/manifest.json').read_text()),
          'gpu_acceptance_sha256': sha(state / 'gpu-acceptance.json') if profile == 'gpu' else None}
(state / 'profile-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
(stage / 'usr/share/doc/t736-gpu-profile/build-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
PY
test "$account_before" = "$(sha256sum "$stage/etc/shadow")"
test "$gdm_before" = "$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")"
unset account_before gdm_before
for old in v4 v6 v7 v15; do (cd "$stage/boot/t736-source-$old-recovery" && sha256sum -c SHA256SUMS); done
if [ -s "$recovery/legacy-tools/SHA256SUMS" ]; then (cd "$recovery/legacy-tools" && sha256sum -c SHA256SUMS); fi
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
