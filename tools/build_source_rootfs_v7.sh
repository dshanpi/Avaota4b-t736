#!/bin/sh
# Offline GNOME image preparation. No board access, flashing, or source-image writes.
# Usage: sudo -n sh tools/build_source_rootfs_v7.sh prepare|resume-prepare|finalize
set -eu
apt_mirror=${T736_V7_APT_MIRROR:-default}
case "$apt_mirror" in default|aliyun) ;; *) printf 'Unsupported build mirror\n' >&2; exit 2 ;; esac
requested_phase=${1:-}
phase=$requested_phase
resume=0
case "$phase" in
    prepare|finalize) ;;
    resume-prepare) phase=prepare; resume=1 ;;
    *) printf 'Usage: %s prepare|resume-prepare|finalize\n' "$0" >&2; exit 2 ;;
esac
[ "$#" -eq 1 ]
[ "$(id -u)" = 0 ]
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]

# Always create our own mount namespace, even when the caller already uses one.
if [ "${T736_V7_IN_NAMESPACE:-0}" != 1 ]; then
    exec unshare --mount --propagation private env T736_V7_IN_NAMESPACE=1 \
        T736_V7_CALLER_NS="$(readlink /proc/self/ns/mnt)" sh "$0" "$requested_phase"
fi
[ "$(readlink /proc/self/ns/mnt)" != "${T736_V7_CALLER_NS:-}" ]
mount --make-rprivate /
mkdir -p "$root/build" "$root/output" "$root/evidence"
exec 9> "$root/build/source-rootfs-v7.lock"
flock -n 9

base="$root/output/rootfs-armbian-source-v6.ext4"
image="$root/output/rootfs-armbian-source-v7.ext4"
stage="$root/build/source-rootfs-v7-mount"
state="$root/build/source-rootfs-v7-state"
private="$root/build/private"
archive="$private/source-v7-preserved-state.tgz"
archive_sha=112f8cce9f34177942894c6eade825d41e03068908d0759aba95f4edcca064c9
password_file="$private/source-v7-password"
previous=6.18.63-t736-hdmi-v13
release=6.18.63-t736-hdmi-v14
compat=6.18.63-t736-armbian
pkg="$root/build/linux-image-${release}_6.18.63-14_arm64.deb"
config="$root/config/desktop-v7"
test -s "$root/config/desktop-v7-packages.txt"
test ! -L "$image"
test ! -L "$stage"
if [ "$phase" = prepare ]; then
    if [ "$resume" = 1 ]; then
        test -f "$image"
        test -d "$state"
        test ! -L "$state"
        test ! -e "$state/prepare.complete"
        test ! -e "$state/finalize.complete"
        test -s "$root/evidence/source-v7-base-rootfs.sha256"
    else
        test ! -e "$image"
        test ! -e "$state"
    fi
    test -f "$base"
    test "$(stat -c %s "$base")" = 7802716160
    test -f "$archive"
    test -f "$password_file"
    test ! -L "$password_file"
    test "$(stat -c %u "$password_file")" = 0
    case "$(stat -c %a "$password_file")" in 400|600) ;; *) printf 'Password file must have mode 400 or 600\n' >&2; exit 1 ;; esac
    printf '%s  %s\n' "$archive_sha" "$archive" | sha256sum -c -
    # Reject archive members outside the three explicitly preserved directories.
    archive_members=$(tar -tzf "$archive")
    printf '%s\n' "$archive_members" | sed 's#^\./##' | awk '
        /^\/$|^\// || /(^|\/)\.\.(\/|$)/ { bad=1 }
        !/^(etc|root|home)(\/|$)/ { bad=1 }
        END { exit bad }
    '
    if [ "$resume" = 0 ]; then
        mkdir -m 700 "$state"
        sha256sum "$base" > "$root/evidence/source-v7-base-rootfs.sha256"
        cp --reflink=auto --sparse=always "$base" "$image"
    fi
else
    test -f "$state/prepare.complete"
    test ! -e "$state/finalize.complete"
    test -f "$image"
    test -s "$pkg"
    test "$(dpkg-deb -f "$pkg" Package)" = "linux-image-$release"
    test "$(dpkg-deb -f "$pkg" Architecture)" = arm64
    test "$(dpkg-deb -f "$pkg" Version)" = 6.18.63-14
fi
test "$(stat -c %s "$image")" = 7802716160
mkdir -p "$stage"
test -z "$(find "$stage" -mindepth 1 -maxdepth 1 -print -quit)"

root_mounted=0
resolv_saved=0
policy_saved=0
resolv_present=0
policy_present=0
mirror_sources_saved=0
cleanup() {
    cleanup_failed=0
    if [ "$root_mounted" = 1 ]; then
        if [ "$mirror_sources_saved" = 1 ]; then
            tar -xpf "$state/$phase.apt-sources.tar" -C "$stage/etc/apt" || cleanup_failed=1
            mirror_sources_saved=0
        fi
        if [ "$resolv_saved" = 1 ]; then
            rm -f -- "$stage/etc/resolv.conf"
            if [ "$resolv_present" = 1 ]; then
                cp -a -- "$state/$phase.resolv.conf" "$stage/etc/resolv.conf" || cleanup_failed=1
            fi
            resolv_saved=0
        fi
        if [ "$policy_saved" = 1 ]; then
            rm -f -- "$stage/usr/sbin/policy-rc.d"
            if [ "$policy_present" = 1 ]; then
                cp -a -- "$state/$phase.policy-rc.d" "$stage/usr/sbin/policy-rc.d" || cleanup_failed=1
            fi
            policy_saved=0
        fi
        for sub in run sys proc dev; do
            if mountpoint -q "$stage/$sub"; then
                umount -R "$stage/$sub" || cleanup_failed=1
            fi
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

if [ "$phase" = prepare ] && [ "$resume" = 0 ]; then
    tar --extract --gzip --file "$archive" --directory "$stage" --numeric-owner \
        --same-permissions --acls --xattrs --xattrs-include='*'
    rm -f -- "$stage/root/t736-source-v6-io-check.sha256"
    recovery="$stage/boot/t736-source-v6-recovery"
    test ! -e "$recovery"
    mkdir "$recovery"
    cp -p "$stage/boot/vmlinuz-$previous" "$stage/boot/initrd.img-$previous" \
        "$stage/boot/config-$previous" "$stage/boot/System.map-$previous" \
        "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb" "$recovery/"
    (cd "$recovery" && sha256sum vmlinuz-* initrd.img-* config-* System.map-* *.dtb > SHA256SUMS)
    (cd "$stage/boot/t736-source-v4-recovery" && sha256sum -c SHA256SUMS)
fi
if [ "$resume" = 1 ]; then
    # Resume only after the preceding invocation has cleaned up its mounts.
    # Both recovery copies must already be complete; never extract or recopy.
    (cd "$stage/boot/t736-source-v6-recovery" && sha256sum -c SHA256SUMS)
    (cd "$stage/boot/t736-source-v4-recovery" && sha256sum -c SHA256SUMS)
fi

if [ -e "$stage/etc/resolv.conf" ] || [ -L "$stage/etc/resolv.conf" ]; then
    cp -a -- "$stage/etc/resolv.conf" "$state/$phase.resolv.conf"
    resolv_present=1
fi
resolv_saved=1
rm -f -- "$stage/etc/resolv.conf"
cp -L -- /etc/resolv.conf "$stage/etc/resolv.conf"
if [ -e "$stage/usr/sbin/policy-rc.d" ] || [ -L "$stage/usr/sbin/policy-rc.d" ]; then
    cp -a -- "$stage/usr/sbin/policy-rc.d" "$state/$phase.policy-rc.d"
    policy_present=1
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
    chroot "$stage" env DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none \
        UCF_FORCE_CONFFOLD=1 "$@"
}

use_build_mirror() {
    if [ "$apt_mirror" = aliyun ]; then
        set -- sources.list.d
        if [ -f "$stage/etc/apt/sources.list" ]; then set -- "$@" sources.list; fi
        tar -cpf "$state/$phase.apt-sources.tar" -C "$stage/etc/apt" "$@"
        mirror_sources_saved=1
        find "$stage/etc/apt" -maxdepth 2 -type f \( -name '*.sources' -o -name '*.list' \) \
            -exec sed -i \
                -e 's#https\?://deb.debian.org/debian#https://mirrors.aliyun.com/debian#g' \
                -e 's#https\?://security.debian.org\(/debian-security\)\?/*#https://mirrors.aliyun.com/debian-security/#g' {} +
        printf 'Using temporary Debian build download mirror; signed sources restored during cleanup\n'
    fi
}

check_manifest_packages() {
    for package_spec in $(sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' "$root/config/desktop-v7-packages.txt"); do
        package_name=${package_spec%%=*}
        test "$(chroot_run dpkg-query -W -f='${Status}' "$package_name")" = 'install ok installed'
        case "$package_spec" in
            *=*) test "$(chroot_run dpkg-query -W -f='${Version}' "$package_name")" = "${package_spec#*=}" ;;
        esac
    done
}

if [ "$phase" = prepare ]; then
    mkdir -p "$stage/etc/apt/preferences.d"
    cp "$config/apt-preferences" "$stage/etc/apt/preferences.d/t736-desktop-v7"
    use_build_mirror
    printf 'gdm3 shared/default-x-display-manager select gdm3\n' | chroot_run debconf-set-selections
    chroot_run apt-get update
    # Only package identifiers from the reviewed project manifest enter this list.
    set -- $(sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' "$root/config/desktop-v7-packages.txt")
    chroot_run apt-get -y --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold install "$@"
    for mesa in libgl1-mesa-dri libegl-mesa0 libglx-mesa0 libgbm1 mesa-libgallium; do
        test "$(chroot_run dpkg-query -W -f='${Version}' "$mesa")" = 25.0.7-2+deb13u1
    done
    for group in sudo audio video render input; do
        chroot_run getent group "$group" >/dev/null || chroot_run groupadd --system "$group"
    done
    if chroot_run getent passwd t736 >/dev/null; then
        test "$(chroot_run id -u t736)" -ge 1000
        test "$(chroot_run getent passwd t736 | cut -d: -f6)" = /home/t736
    else
        chroot_run useradd --create-home --user-group --shell /bin/bash t736
    fi
    chroot_run usermod --append --groups sudo,audio,video,render,input --shell /bin/bash t736
    desktop_password=$(cat "$password_file")
    case "$desktop_password" in ''|*:*|*'
'*) printf 'Password input must contain one nonempty line without colons\n' >&2; exit 1 ;; esac
    printf 'root:%s\nt736:%s\n' "$desktop_password" "$desktop_password" | chroot_run chpasswd
    unset desktop_password
    for account in root t736; do
        chroot_run chage -I -1 -m 0 -M -1 -E -1 "$account"
    done
    for locale in 'en_US.UTF-8 UTF-8' 'zh_CN.UTF-8 UTF-8'; do
        grep -qx "$locale" "$stage/etc/locale.gen" || printf '%s\n' "$locale" >> "$stage/etc/locale.gen"
    done
    chroot_run locale-gen
    chroot_run update-locale --reset LANG=zh_CN.UTF-8 LANGUAGE=zh_CN:zh
    mkdir -p "$stage/etc/gdm3" "$stage/etc/X11/xorg.conf.d" "$stage/var/lib/AccountsService/users"
    cp "$config/gdm3-daemon.conf" "$stage/etc/gdm3/daemon.conf"
    cp "$config/20-t736-modesetting.conf" "$stage/etc/X11/xorg.conf.d/20-t736-modesetting.conf"
    printf '[User]\nXSession=gnome-xorg\nLanguage=zh_CN.UTF-8\nSystemAccount=false\n' \
        > "$stage/var/lib/AccountsService/users/t736"
    chmod 600 "$stage/var/lib/AccountsService/users/t736"
    printf '[Desktop]\nSession=gnome-xorg\nLanguage=zh_CN.UTF-8\n' > "$stage/home/t736/.dmrc"
    chroot_run chown t736: /home/t736/.dmrc
    touch "$stage/etc/environment"
    sed -i '/^LIBGL_ALWAYS_SOFTWARE=/d; /^GALLIUM_DRIVER=/d' "$stage/etc/environment"
    printf '\nLIBGL_ALWAYS_SOFTWARE=true\nGALLIUM_DRIVER=llvmpipe\n' >> "$stage/etc/environment"
    test -f "$stage/usr/share/xsessions/gnome-xorg.desktop"
    grep -Eq 'pam_succeed_if\.so[[:space:]]+user[[:space:]]+!=[[:space:]]+root' "$stage/etc/pam.d/gdm-password"
    # Units are enabled offline; policy-rc.d has prevented service startup.
    chroot_run systemctl enable gdm.service
    chroot_run systemctl enable serial-getty@ttyS0.service
    chroot_run systemctl enable systemd-timesyncd.service
    chroot_run systemctl set-default graphical.target
fi

# Kernel finalization is separate so GNOME can be prepared while v14 compiles.
if [ "$phase" = finalize ]; then
    # These additions can be finalized after an earlier prepare has completed.
    extras_ready=1
    for extra in gnome-backgrounds ethtool dconf-cli; do
        if [ "$(chroot_run dpkg-query -W -f='${Status}' "$extra" 2>/dev/null || true)" != 'install ok installed' ]; then extras_ready=0; fi
    done
    if [ "$extras_ready" != 1 ]; then
        use_build_mirror
        chroot_run apt-get update
        chroot_run apt-get -y --no-install-recommends \
            -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
            install gnome-backgrounds ethtool dconf-cli
    fi
    check_manifest_packages
    mkdir -p "$stage/etc/dconf/db/t736.d" "$stage/etc/dconf/profile"
    cat > "$stage/etc/dconf/db/t736.d/00-input-sources" <<'EOF'
[org/gnome/desktop/input-sources]
sources=[('xkb', 'us'), ('ibus', 'libpinyin')]
EOF
    # Keep existing profile entries and per-user overrides; only add defaults.
    if [ ! -s "$stage/etc/dconf/profile/user" ]; then
        printf 'user-db:user\n' > "$stage/etc/dconf/profile/user"
    fi
    grep -qx 'system-db:t736' "$stage/etc/dconf/profile/user" || \
        printf '\nsystem-db:t736\n' >> "$stage/etc/dconf/profile/user"
    chroot_run dconf update
    chroot_run update-locale --reset LANG=zh_CN.UTF-8 LANGUAGE=zh_CN:zh
    test "$(chroot_run systemctl get-default)" = graphical.target
    mkdir -p "$stage/etc/systemd/resolved.conf.d"
    cp "$config/resolved-dns-over-tls.conf" "$stage/etc/systemd/resolved.conf.d/60-t736-desktop.conf"
    chroot_run systemctl enable systemd-resolved.service
    # Debian's GDM unit is static; offline enable alone does not create its alias.
    test -f "$stage/usr/lib/systemd/system/gdm.service"
    test ! -e "$stage/etc/systemd/system/display-manager.service" || \
        test -L "$stage/etc/systemd/system/display-manager.service"
    ln -sfn /usr/lib/systemd/system/gdm.service "$stage/etc/systemd/system/display-manager.service"
    printf '/usr/sbin/gdm3\n' > "$stage/etc/X11/default-display-manager"
    test -L "$stage/etc/systemd/system/display-manager.service"
    display_manager=$(chroot_run readlink -f /etc/systemd/system/display-manager.service)
    case "$display_manager" in */gdm.service|*/gdm3.service) ;; *) printf 'GDM display-manager alias missing\n' >&2; exit 1 ;; esac
    printf 'Default desktop service: %s\n' "$display_manager"
    if [ "$(chroot_run dpkg-query -W -f='${Status} ${Version}' "linux-image-$release" 2>/dev/null || true)" != 'install ok installed 6.18.63-14' ] ||
            [ ! -s "$stage/boot/initrd.img-$release" ]; then
        test ! -e "$stage/tmp/t736-source-v7-kernel.deb"
        cp "$pkg" "$stage/tmp/t736-source-v7-kernel.deb"
        chroot_run dpkg -i /tmp/t736-source-v7-kernel.deb
        rm -f -- "$stage/tmp/t736-source-v7-kernel.deb"
    fi
    test "$(chroot_run dpkg-query -W -f='${Version}' "linux-image-$release")" = 6.18.63-14
    # Armbian's initramfs hook can remove the legacy alias as an unused version.
    test ! -e "$stage/boot/vmlinuz-$compat" || test -L "$stage/boot/vmlinuz-$compat"
    test ! -e "$stage/boot/initrd.img-$compat" || test -L "$stage/boot/initrd.img-$compat"
    ln -sfn "vmlinuz-$release" "$stage/boot/vmlinuz-$compat"
    ln -sfn "initrd.img-$release" "$stage/boot/initrd.img-$compat"
    cp "$stage/usr/lib/linux-image-$release/allwinner/t736-hdmi-candidate.dtb" \
        "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
    chroot_run lsinitramfs "/boot/initrd.img-$release" > "$root/output/initrd-source-v7.contents"
    grep -Eq '(^|/)(sbin|bin)/fsck\.ext4$' "$root/output/initrd-source-v7.contents"
    gzip -dc "$stage/boot/vmlinuz-$release" | cmp - "$root/build/linux-hdmi-v14/arch/arm64/boot/Image"
    cmp "$root/build/linux-hdmi-v14/arch/arm64/boot/dts/allwinner/t736-hdmi-candidate.dtb" \
        "$stage/boot/dtb/allwinner/t736-mainline-bringup.dtb"
    (cd "$stage/boot" && sha256sum "vmlinuz-$release" "initrd.img-$release" \
        dtb/allwinner/t736-mainline-bringup.dtb) > "$root/evidence/source-v7-boot-file-hashes.sha256"
    # Seed the first boot's clock; systemd-timesyncd subsequently corrects it.
    test -f "$stage/etc/fake-hwclock.data"
    test ! -L "$stage/etc/fake-hwclock.data"
    LC_ALL=C date -u '+%Y-%m-%d %H:%M:%S' > "$stage/etc/fake-hwclock.data"
    chown 0:0 "$stage/etc/fake-hwclock.data"
    chmod 644 "$stage/etc/fake-hwclock.data"
    cp -p "$stage/etc/fake-hwclock.data" "$root/evidence/source-v7-build-clock-utc.txt"
fi
chroot_run apt-get check
chroot_run dpkg --audit > "$state/$phase.dpkg-audit"
test ! -s "$state/$phase.dpkg-audit"
(cd "$stage/boot/t736-source-v6-recovery" && sha256sum -c SHA256SUMS)
(cd "$stage/boot/t736-source-v4-recovery" && sha256sum -c SHA256SUMS)
chroot_run dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' \
    > "$root/output/source-v7-dpkg-$phase.tsv"
chroot_run apt-get clean
df -B1 "$stage" > "$root/evidence/source-v7-$phase-space.txt"
sync
cleanup
trap - EXIT HUP INT TERM
if e2fsck -fn "$image" > "$root/evidence/source-v7-$phase-fsck.log" 2>&1; then
    cat "$root/evidence/source-v7-$phase-fsck.log"
else
    cat "$root/evidence/source-v7-$phase-fsck.log" >&2
    exit 1
fi
printf '%s complete\n' "$phase" > "$state/$phase.complete"
if [ "$phase" = finalize ]; then
    sha256sum "$image" > "$root/output/rootfs-armbian-source-v7.ext4.sha256"
fi
printf 'SOURCE_V7_%s_READY\n' "$phase"
