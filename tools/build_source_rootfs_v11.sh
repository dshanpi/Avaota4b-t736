#!/bin/sh
# DRAFT: offline source-v11 from frozen v10. Not executed or image-accepted.
# Check is the default. Build requires an explicit reviewed acceptance manifest.
# No board access, raw device write, password change, auto-login or Mutter trial.
set -eu
mode=check
case "${1:-}" in --check) shift ;; --build) mode=build; shift ;; esac
[ "$#" -eq 2 ] && [ "$1" = --manifest ] || {
    printf 'Usage: %s [--check|--build] --manifest /abs/reviewed-inputs.json\n' "$0" >&2; exit 2;
}
manifest=$(realpath -e -- "$2")
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
plan=$(python3 "$root/tools/validation/source_v11_plan.py" --root "$root" --manifest "$manifest")
if [ "$mode" = check ]; then
    printf '%s\n' "$plan"
    exit 0
fi
[ "$(id -u)" = 0 ]
if [ "${T736_V11_IN_NAMESPACE:-0}" != 1 ]; then
    exec unshare --mount --propagation private env T736_V11_IN_NAMESPACE=1 \
        T736_V11_CALLER_NS="$(readlink /proc/self/ns/mnt)" sh "$0" --build --manifest "$manifest"
fi
[ -n "${T736_V11_CALLER_NS:-}" ]
[ "$(readlink /proc/self/ns/mnt)" != "$T736_V11_CALLER_NS" ]
mount --make-rprivate /
state="$root/build/source-rootfs-v11-state"
stage="$root/build/source-rootfs-v11-mount"
image="$root/output/rootfs-armbian-source-v11.ext4"
exec 9> "$root/build/source-rootfs-v11.lock"
flock -n 9
test ! -e "$state" && test ! -L "$state"
test ! -e "$image" && test ! -L "$image"
test ! -L "$stage"
mkdir -p "$stage"
test -z "$(find "$stage" -mindepth 1 -maxdepth 1 -print -quit)"
mkdir -m 700 "$state"
printf '%s\n' "$plan" > "$state/inputs.json"
field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$state/inputs.json" "$1"; }
release=$(field kernel_release)
version=$(field package_version)
base=$(field base_rootfs)
candidate_build=$(field candidate_build)
# Freeze all mutable input paths before the image is created.
for item in kernel pvr gpu_acceptance storage_acceptance gstreamer_gl_deb recovery_dtb; do
    input=$(field "$item")
    cp -- "$input" "$state/$item.input"
    cmp -- "$input" "$state/$item.input"
done
cp --reflink=auto --sparse=always "$base" "$image"
mounted=0
policy_saved=0
policy_present=0
hooks=
cleanup() {
    failed=0
    if [ "$mounted" = 1 ]; then
        for hook in $hooks; do umount "$stage$hook" || failed=1; done
        hooks=
        if [ "$policy_saved" = 1 ]; then
            rm -f -- "$stage/usr/sbin/policy-rc.d"
            if [ "$policy_present" = 1 ]; then cp -a -- "$state/policy-rc.d.before" "$stage/usr/sbin/policy-rc.d" || failed=1; fi
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
previous=6.18.63-t736-storage-v16
recovery="$stage/boot/t736-source-v16-recovery"
test ! -e "$recovery"
mkdir -m 700 "$recovery"
for stem in vmlinuz initrd.img config System.map; do
    cp -p -- "$stage/boot/$stem-$previous" "$recovery/"
done
install -m 644 "$state/recovery_dtb.input" "$recovery/t736-mainline-bringup.dtb"
(cd "$recovery" && sha256sum vmlinuz-* initrd.img-* config-* System.map-* *.dtb > SHA256SUMS)
(cd "$stage/usr/lib/modules/$previous" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$state/v16-modules.sha256"
# Do not expose credential contents or credential-file hashes in public records.
credentials_before=$(sha256sum "$stage/etc/passwd" "$stage/etc/shadow" "$stage/etc/group" "$stage/etc/gshadow")
gdm_before=$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")
python3 - "$stage" <<'PY'
from pathlib import Path
import configparser,sys
r=Path(sys.argv[1]);c=configparser.ConfigParser();c.read(r/'etc/gdm3/daemon.conf')
assert not c.getboolean('daemon','AutomaticLoginEnable',fallback=False),'Base enables automatic login'
assert not (r/'var/lib/t736-gpu-profile/pending.json').exists()
assert not (r/'root/t736-gpu-session-trial/active').exists()
PY
mkdir -m 700 "$state/empty-hooks"
for hook in /etc/kernel /usr/share/kernel /etc/initramfs/post-update.d; do
    if [ -d "$stage$hook" ]; then
        mount --bind "$state/empty-hooks" "$stage$hook"
        hooks="$hook $hooks"
    fi
done
if [ -e "$stage/usr/sbin/policy-rc.d" ] || [ -L "$stage/usr/sbin/policy-rc.d" ]; then
    cp -a -- "$stage/usr/sbin/policy-rc.d" "$state/policy-rc.d.before"; policy_present=1
fi
policy_saved=1
rm -f -- "$stage/usr/sbin/policy-rc.d"
printf '#!/bin/sh\nexit 101\n' > "$stage/usr/sbin/policy-rc.d"
chmod 755 "$stage/usr/sbin/policy-rc.d"
mount --rbind /dev "$stage/dev"
mount --make-rslave "$stage/dev"
mount -t proc proc "$stage/proc"
mount -t sysfs -o ro sysfs "$stage/sys"
mount -t tmpfs tmpfs "$stage/run"
mkdir -p "$stage/run/lock"
chroot_run() { chroot "$stage" env DEBIAN_FRONTEND=noninteractive "$@"; }
test ! -e "$stage/tmp/t736-v11-kernel.deb" && test ! -e "$stage/tmp/t736-v11-gstgl.deb"
cp -- "$state/kernel.input" "$stage/tmp/t736-v11-kernel.deb"
cp -- "$state/gstreamer_gl_deb.input" "$stage/tmp/t736-v11-gstgl.deb"
# Exact local packages only: no apt upgrade or unreviewed desktop/Mutter update.
chroot_run dpkg -i /tmp/t736-v11-kernel.deb /tmp/t736-v11-gstgl.deb
rm -f -- "$stage/tmp/t736-v11-kernel.deb" "$stage/tmp/t736-v11-gstgl.deb"
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' "linux-image-$release")" = "install ok installed $version"
test "$(chroot_run dpkg-query -W -f='${Status} ${Version}' gstreamer1.0-gl)" = 'install ok installed 1.26.2-1+deb13u2'
test -s "$stage/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstopengl.so"
mkdir -p "$stage/usr/lib/modules/$release/extra"
install -m 644 "$state/pvr.input" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
cmp "$state/pvr.input" "$stage/usr/lib/modules/$release/extra/pvrsrvkm.ko"
chroot_run depmod -a "$release"
if [ -s "$stage/boot/initrd.img-$release" ]; then
    chroot_run update-initramfs -u -k "$release"
else
    chroot_run update-initramfs -c -k "$release"
fi
test "$(chroot_run modinfo -k "$release" -F vermagic pvrsrvkm)" = "$release SMP mod_unload aarch64"
gzip -dc "$stage/boot/vmlinuz-$release" | cmp - "$candidate_build/arch/arm64/boot/Image"
cmp "$stage/boot/config-$release" "$candidate_build/.config"
cmp "$stage/boot/System.map-$release" "$candidate_build/System.map"
cmp "$stage/usr/lib/linux-image-$release/allwinner/t736-gpu-candidate.dtb" "$candidate_build/arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb"
python3 - "$stage" "$release" <<'PY'
from pathlib import Path
import ast,os,sys
r=Path(sys.argv[1]);release=sys.argv[2]
for stem in ('vmlinuz','initrd.img'):
 p=r/'boot'/(stem+'-6.18.63-t736-armbian');q=p.with_name(p.name+'.v11-new')
 assert p.is_symlink() and os.readlink(p)==stem+'-6.18.63-t736-storage-v16'
 assert not q.exists() and not q.is_symlink();q.symlink_to(stem+'-'+release);os.replace(q,p)
p=r/'boot/dtb/allwinner/t736-mainline-bringup.dtb';q=p.with_name(p.name+'.v11-new')
assert not q.exists()
with q.open('xb') as f:
 f.write((r/'usr/lib'/('linux-image-'+release)/'allwinner/t736-gpu-candidate.dtb').read_bytes());f.flush();os.fsync(f.fileno())
os.replace(q,p)
for name in ('t736-gpu-profile','t736-gpu-runtime-check'):
 p=r/'usr/local/sbin'/name;text=p.read_text();old='6.18.63-t736-storage-v16'
 assert text.count(old)==1
 (r/'boot/t736-source-v16-recovery'/name).write_bytes(p.read_bytes())
 changed=text.replace(old,release)
 if name=='t736-gpu-profile':ast.parse(changed)
 p.write_text(changed)
for p in (r/'boot',r/'boot/dtb/allwinner'):
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY)
 try:os.fsync(fd)
 finally:os.close(fd)
PY
# The existing v10 GPU settings are re-established through the same reviewed
# profile logic and exact new-kernel acceptance. No Mutter binary is replaced.
python3 "$stage/usr/local/sbin/t736-gpu-profile" software --root "$stage"
python3 "$stage/usr/local/sbin/t736-gpu-profile" gpu --root "$stage" --evidence "$state/gpu_acceptance.input"
test "$credentials_before" = "$(sha256sum "$stage/etc/passwd" "$stage/etc/shadow" "$stage/etc/group" "$stage/etc/gshadow")"
test "$gdm_before" = "$(sha256sum "$stage/etc/gdm3/daemon.conf" "$stage/etc/pam.d/gdm-password")"
unset credentials_before gdm_before
(cd "$recovery" && sha256sum -c SHA256SUMS)
(cd "$stage/usr/lib/modules/$previous" && sha256sum -c "$state/v16-modules.sha256")
chroot_run lsinitramfs "/boot/initrd.img-$release" > "$state/initrd.contents"
grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$state/initrd.contents"
grep -F "lib/modules/$release/" "$state/initrd.contents" >/dev/null
test "$(chroot_run systemctl get-default)" = graphical.target
chroot_run dpkg --audit > "$state/dpkg-audit"
test ! -s "$state/dpkg-audit"
chroot_run apt-get check
python3 - "$stage" "$state" "$release" <<'PY'
from pathlib import Path
import hashlib,json,sys
r,s=map(Path,sys.argv[1:3]);release=sys.argv[3]
plan=json.loads((s/'inputs.json').read_text())
paths=['/boot/vmlinuz-'+release,'/boot/initrd.img-'+release,'/boot/config-'+release,
 '/boot/dtb/allwinner/t736-mainline-bringup.dtb','/usr/lib/modules/'+release+'/extra/pvrsrvkm.ko',
 '/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstopengl.so','/usr/local/sbin/t736-gpu-profile',
 '/usr/local/sbin/t736-gpu-runtime-check','/etc/environment','/etc/X11/xorg.conf.d/20-t736-modesetting.conf']
record={'schema':1,'variant':'v11','kernel_release':release,'desktop_profile':'gpu',
 'storage_mode':plan['storage_mode'],'input_manifest_sha256':plan['manifest_sha256'],
 'persistent_bootargs_required':plan['boot_parameters'],'bootcommand':plan['bootcommand'],
 'installed_files':{p:hashlib.sha256((r/p.lstrip('/')).read_bytes()).hexdigest() for p in paths},
 'credentials_preserved':True,'gdm_auto_login':False,'mutter_experiment_included':False,
 'explicit_recovery_kernel':'6.18.63-t736-storage-v16','automatic_kernel_hang_fallback':False,
 'final_image_cold_boot_validated':False,'new_uboot_payload_cold_boot_validated':False}
text=json.dumps(record,indent=2)+'\n';(s/'profile-manifest.json').write_text(text)
(r/'usr/share/doc/t736-gpu-profile/build-manifest.json').write_text(text)
PY
sync
cleanup
trap - EXIT HUP INT TERM
e2fsck -fn "$image" > "$state/finalize-fsck.log" 2>&1 || { cat "$state/finalize-fsck.log" >&2; exit 1; }
sha256sum "$image" > "$image.sha256"
printf 'finalize complete\n' > "$state/finalize.complete"
printf 'SOURCE_V11_ROOTFS_BUILT_NOT_COLD_BOOT_ACCEPTED\n'
