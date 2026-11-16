#!/bin/bash
# Reversible v15 desktop experiment; run from the root serial console.
# FIRST validate the v15 kernel/IMG module and GPU rendering/readback probes.
# enable does not establish GPU or physical-display acceptance. Keep root access.
# Usage: bash gpu_session_trial.sh {enable|rollback|status}
set -Eeuo pipefail
umask 077

STATE=/root/t736-gpu-session-trial
ENV_FILE=/etc/environment
XORG_FILE=/etc/X11/xorg.conf.d/20-t736-modesetting.conf
DROPIN=/run/systemd/system/gdm.service.d/90-t736-gpu-session-trial.conf
PREFIX=/opt/t736-img-ddk-24.2-x11
RELEASE=6.18.63-t736-gpu-v15
run=
rollback_needed=0

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }
[[ $# == 1 ]] || die 'Usage: gpu_session_trial.sh enable|rollback|status'
case "$1" in enable|rollback|status) action=$1 ;; *) die 'Unknown action' ;; esac
[[ $EUID == 0 ]] || die 'Run as root from the root serial console.'
[[ ! -L $STATE ]] || die 'State directory must not be a symlink.'
mkdir -p "$STATE/runs"
chmod 700 "$STATE" "$STATE/runs"
[[ ! -L $STATE/lock && ! -L $STATE/active ]] || die 'Invalid state files.'
exec 9>"$STATE/lock"
flock -n 9 || die 'Another GPU session operation is running.'

load_active() {
    local name
    [[ -f $STATE/active ]] || return 1
    read -r name < "$STATE/active"
    [[ $name =~ ^[0-9]{8}T[0-9]{6}Z-[0-9]+$ ]] || die 'Invalid active run.'
    run=$STATE/runs/$name
    [[ -d $run && ! -L $run && -f $run/backup.complete ]] || die 'Incomplete backup; inspect state before proceeding.'
}

status() {
    if load_active; then
        printf 'Active trial: %s\n' "$run"
        [[ ! -f $run/phase ]] || cat "$run/phase"
    else
        note 'No active trial.'
    fi
    printf 'Kernel: %s\n' "$(uname -r)"
    systemctl show gdm.service -p ActiveState -p SubState -p MainPID --no-pager || true
    if [[ -f $XORG_FILE ]]; then grep -E 'AccelMethod|PageFlip|kmsdev' "$XORG_FILE" || true; fi
    printf 'Trial drop-in present: '; [[ -f $DROPIN ]] && echo yes || echo no
    note 'Status is not a hardware-rendering acceptance result.'
}

# Neither sudo from the GUI user nor a root shell inside its session is safe:
# terminating that user would also terminate this transaction and its rollback.
guard_controller() {
    local user uid
    for user in t736 Debian-gdm; do
        uid=$(id -u "$user") || return 1
        [[ $uid =~ ^[0-9]+$ && $uid != 0 ]] || return 1
        [[ ${SUDO_USER:-} != "$user" ]] || return 1
        if grep -q "/user-${uid}\.slice\(/\|$\)" /proc/self/cgroup; then return 1; fi
    done
}

diagnose() {
    local label=$1
    {
        date -u --iso-8601=seconds
        uname -a
        systemctl show gdm.service -p ActiveState -p SubState -p MainPID --no-pager
        loginctl list-sessions --no-pager
        journalctl -b -u gdm.service -n 150 --no-pager
        journalctl -b -k -n 100 --no-pager
    } > "$run/$label.log" 2>&1 || true
}

stop_sessions() {
    local user uid state failed=0
    systemctl stop gdm.service || failed=1
    for user in t736 Debian-gdm; do
        uid=$(id -u "$user") || { failed=1; continue; }
        [[ $uid =~ ^[0-9]+$ && $uid != 0 ]] || { failed=1; continue; }
        if loginctl show-user "$uid" >/dev/null 2>&1; then
            loginctl terminate-user "$uid" || failed=1
        fi
        systemctl stop "user@$uid.service" "user-runtime-dir@$uid.service" || failed=1
        state=$(systemctl show "user@$uid.service" -p ActiveState --value) || failed=1
        case "$state" in inactive|failed) ;; *) failed=1 ;; esac
    done
    return "$failed"
}

save_file() {
    local name=$1 path=$2
    [[ ! -L $path ]] || die "Refusing symlink: $path"
    if [[ -e $path ]]; then
        [[ -f $path ]] || die "Not a regular file: $path"
        cp -a -- "$path" "$run/$name.original"
        printf 'present\n' > "$run/$name.state"
    else
        printf 'absent\n' > "$run/$name.state"
    fi
}

# Keep the original mode/owner; replace each file atomically on its own filesystem.
install_file() {
    local source=$1 dest=$2 tmp
    tmp=$(mktemp "${dest}.t736-trial.XXXXXX") || return 1
    if ! cp -a -- "$source" "$tmp" || ! mv -fT -- "$tmp" "$dest"; then
        rm -f -- "$tmp"
        return 1
    fi
}

restore_file() {
    local name=$1 path=$2 presence
    read -r presence < "$run/$name.state" || return 1
    case "$presence" in
        present) install_file "$run/$name.original" "$path" ;;
        absent) rm -f -- "$path" ;;
        *) return 1 ;;
    esac
}

rollback_internal() {
    local failed=0
    note "Restoring baseline from $run"
    diagnose before-rollback
    stop_sessions || failed=1
    restore_file environment "$ENV_FILE" || failed=1
    restore_file xorg "$XORG_FILE" || failed=1
    mkdir -p "$(dirname "$DROPIN")" || failed=1
    restore_file dropin "$DROPIN" || failed=1
    systemctl daemon-reload || failed=1
    systemctl reset-failed gdm.service || true
    systemctl start gdm.service || failed=1
    systemctl is-active --quiet gdm.service || failed=1
    diagnose after-rollback
    if [[ $failed == 0 ]]; then
        printf 'rolled-back\n' > "$run/phase"
        rm -f -- "$STATE/active"
        rollback_needed=0
        note "Baseline restored; diagnostics retained in $run"
        return 0
    fi
    printf 'rollback-needs-attention\n' > "$run/phase"
    note "ERROR: rollback incomplete; retain root console and rerun rollback. See $run" >&2
    return 1
}

on_exit() {
    local rc=$?
    trap - EXIT INT TERM HUP
    if [[ $rollback_needed == 1 ]]; then
        # A failed enable must not deliberately leave candidate files deployed.
        rollback_internal || rc=1
    fi
    exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

preflight() {
    local module
    [[ $(uname -r) == "$RELEASE" ]] || die "Requires $RELEASE; refusing another kernel."
    [[ -f /boot/t736-source-v7-recovery/SHA256SUMS ]] || die 'Missing v8 baseline recovery record.'
    [[ $(dpkg-query -W -f='${Status} ${Version}' t736-img-userspace-candidate) == 'install ok installed 24.2.6603887-2' ]] || die 'IMG package/version mismatch.'
    [[ -d /sys/module/pvrsrvkm ]] || die 'Validate/load the IMG module before this desktop trial.'
    module=$(modinfo -n pvrsrvkm)
    [[ $(modinfo -F vermagic pvrsrvkm) == "$RELEASE "* ]] || die 'Module vermagic mismatch.'
    python3 - "$module" <<'PY' || die 'Module DDK mismatch.'
import pathlib, sys
signature = b'Rogue_DDK_Linux_WS rogueddk 24.2@6603887\0'
if signature not in pathlib.Path(sys.argv[1]).read_bytes():
    raise SystemExit('Expected DDK version string is absent')
PY
    [[ -f $PREFIX/SHA256SUMS && -x $PREFIX/bin/gpu-probe ]] || die 'Missing isolated GPU package.'
    printf '%s  %s\n' 9ae49ed015dd2fafc55361eeaab3f23789f9b42f0f18a36eb341cb1ff3b02a69 "$PREFIX/manifest.json" \
        | sha256sum -c - > "$run/manifest-check.log" 2>&1 || die 'GPU manifest mismatch.'
    python3 - "$PREFIX/manifest.json" <<'PY'
import json, os, sys
from pathlib import Path
manifest = json.loads(Path(sys.argv[1]).read_text())
for item in manifest['files']:
    if 'symlink' in item:
        path = '/' + item['path']
        if not os.path.islink(path) or os.readlink(path) != item['symlink']:
            raise SystemExit('GPU payload symlink mismatch: ' + path)
PY
    sha256sum -c "$PREFIX/SHA256SUMS" > "$run/payload-check.log" 2>&1 || die 'GPU payload checksum mismatch.'
    python3 "$PREFIX/bin/gpu-runtime-view" --verify > "$run/runtime-view-check.log" || die 'GPU runtime view mismatch.'
    printf '%s  %s\n' \
        43eb5d054ee98bc975274198d2ed440e2f0f50b7ffcd0996a4022949f2c7909f /usr/lib/firmware/rgx.fw.36.56.104.183 \
        71e1b1da9b4c6b42468e378e8b835306920568088bc6669879db4a108a7747c9 /usr/lib/firmware/rgx.sh.36.56.104.183 \
        | sha256sum -c - > "$run/firmware-check.log" 2>&1 || die 'GPU firmware mismatch.'
    [[ -c /dev/dri/card0 ]] || die 'Missing display DRM node.'
    compgen -G '/dev/dri/renderD*' >/dev/null || die 'Missing render node; validate the GPU first.'
    [[ -f $ENV_FILE && -f $XORG_FILE ]] || die 'Missing baseline environment/Xorg file.'
    [[ $(readlink -f /etc/systemd/system/display-manager.service) == /usr/lib/systemd/system/gdm.service ]] || die 'Unexpected display manager.'
    guard_controller || die 'Use an independent root console; GUI users/root aliases are not allowed.'
}

case "$action" in
    status) status; exit 0 ;;
    rollback)
        if ! load_active; then note 'No active trial to roll back.'; exit 0; fi
        guard_controller || die 'Rollback requires an independent root console.'
        rollback_internal
        exit 0
        ;;
    enable)
        if load_active; then die "An existing trial needs status/rollback first: $run"; fi
        run=$STATE/runs/$(date -u +%Y%m%dT%H%M%SZ)-$$
        mkdir -m 700 "$run"
        preflight
        save_file environment "$ENV_FILE"
        save_file xorg "$XORG_FILE"
        save_file dropin "$DROPIN"
        # Exact v7 configuration; no guesses about another Xorg device section.
        python3 - "$run" "$PREFIX" <<'PY'
from pathlib import Path
import hashlib, re, shutil, sys
run, prefix = Path(sys.argv[1]), sys.argv[2]
expected = '''Section "Device"
    Identifier "T736 display engine"
    Driver "modesetting"
    Option "kmsdev" "/dev/dri/card0"
    Option "AccelMethod" "none"
    Option "SWcursor" "true"
    Option "PageFlip" "false"
EndSection
'''
xorg = (run / 'xorg.original').read_text()
if xorg != expected:
    raise SystemExit('Unexpected 20-t736-modesetting.conf; refusing to change it')
shutil.copy2(run / 'xorg.original', run / 'xorg.candidate')
resource = Path('/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf')
if not resource.is_file() or resource.is_symlink():
    raise SystemExit('Missing reviewed 1080p Xorg configuration')
candidate = resource.read_bytes()
if hashlib.sha256(candidate).hexdigest() != 'ad0e2b03a3b31450a69a272f437760ea1f2adaffb77a6e15703c3b5f1c3c101d':
    raise SystemExit('1080p Xorg configuration differs from reviewed artifact')
(run / 'xorg.candidate').write_bytes(candidate)
remove = {'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER', 'MESA_LOADER_DRIVER_OVERRIDE',
          'LD_PRELOAD', '__EGL_VENDOR_LIBRARY_FILENAMES', '__GLX_VENDOR_LIBRARY_NAME',
          'LD_LIBRARY_PATH', 'LIBGL_DRIVERS_PATH', 'COGL_DRIVER',
          'VK_ICD_FILENAMES', 'VK_DRIVER_FILES', 'VK_ADD_DRIVER_FILES',
          'GDK_BACKEND', 'GDK_DISABLE', 'GSK_RENDERER'}
lines = []
for line in (run / 'environment.original').read_text().splitlines(keepends=True):
    match = re.match(r'^\s*(?:export\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*=', line)
    if not match or match.group(1) not in remove:
        lines.append(line)
text = ''.join(lines)
if text and not text.endswith('\n'):
    text += '\n'
text += (f'LD_LIBRARY_PATH={prefix}/runtime-gles\n'
         f'LIBGL_DRIVERS_PATH={prefix}/lib/dri\nCOGL_DRIVER=gles2\n'
         'GDK_BACKEND=x11\nGDK_DISABLE=gl-api,glx,vulkan\nGSK_RENDERER=opengl\n')
shutil.copy2(run / 'environment.original', run / 'environment.candidate')
(run / 'environment.candidate').write_text(text)
PY
        printf '[Service]\nEnvironmentFile=%s\nUnsetEnvironment=LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE LD_PRELOAD __EGL_VENDOR_LIBRARY_FILENAMES __GLX_VENDOR_LIBRARY_NAME VK_ICD_FILENAMES VK_DRIVER_FILES VK_ADD_DRIVER_FILES\n' "$ENV_FILE" > "$run/dropin.candidate"
        chmod 644 "$run/dropin.candidate"
        printf 'backup complete\n' > "$run/backup.complete"
        printf '%s\n' "${run##*/}" > "$STATE/active.new"
        mv -fT "$STATE/active.new" "$STATE/active"
        rollback_needed=1
        printf 'enabling\n' > "$run/phase"
        diagnose before-enable
        stop_sessions
        install_file "$run/environment.candidate" "$ENV_FILE"
        install_file "$run/xorg.candidate" "$XORG_FILE"
        mkdir -p "$(dirname "$DROPIN")"
        install_file "$run/dropin.candidate" "$DROPIN"
        systemctl daemon-reload
        systemctl reset-failed gdm.service || true
        systemctl start gdm.service
        systemctl is-active --quiet gdm.service
        diagnose after-enable
        printf 'enabled-unverified\n' > "$run/phase"
        rollback_needed=0
        note "Candidate enabled; PageFlip remains false. Diagnostics: $run"
        note 'Verify actual Xorg/GNOME process libraries and hardware renderer; use rollback on failure.'
        ;;
esac
