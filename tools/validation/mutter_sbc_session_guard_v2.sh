#!/bin/bash
# Prepare one temporary, UID-scoped Mutter SBC trial. Does not start a session.
# Run as root after separately verifying/extracting the candidate /opt archive.
set -euo pipefail

state=/run/t736-sbc-ac42-gi2
dropin=/run/systemd/user/org.gnome.Shell@x11.service.d/91-t736-sbc-ac42-gi2.conf
unit=t736-mutter-sbc-ac42-gi2-rollback
prefix=/opt/t736-mutter-sbc-48.7-ac42f10a
libdir=$prefix/lib/aarch64-linux-gnu

if [ "$(id -u)" -ne 0 ]; then
    printf 'Run as root.\n' >&2
    exit 2
fi

case "${1:-}" in
arm)
    seconds=${2:-300}
    case "$seconds" in ''|*[!0-9]*) exit 2;; esac
    [ "$seconds" -ge 60 ] && [ "$seconds" -le 900 ]
    [ "$(getent passwd 1000 | cut -d: -f1)" = t736 ]
    [ ! -e "$state" ] && [ ! -L "$state" ]
    [ ! -e "$dropin" ] && [ ! -L "$dropin" ]
    [ ! -L "${dropin%/*}" ]
    [ "$(systemctl show -p LoadState --value "$unit.timer")" = not-found ]
    [ "$(systemctl show -p LoadState --value "$unit.service")" = not-found ]
    [ -x "$prefix/bin/mutter" ]
    [ -x "$prefix/bin/gnome-shell" ]
    [ -x "$prefix/bin/gnome-shell-sbc-dispatch" ]
    [ ! -e /run/systemd/user/org.gnome.Shell@x11.service.d/90-t736-sbc-ac42.conf ]
    [ -f "$libdir/libmutter-16.so.0" ]
    [ -f "$libdir/mutter-16/libmutter-cogl-16.so.0" ]
    [ -f "$libdir/mutter-16/libmutter-clutter-16.so.0" ]
    [ -f "$libdir/mutter-16/libmutter-mtk-16.so.0" ]
    [ -f /etc/gdm3/daemon.conf ] && [ ! -L /etc/gdm3/daemon.conf ]
    systemctl is-active --quiet display-manager.service

    # The environment file is public configuration; the saved GDM file is private.
    mkdir -m 0755 "$state"
    mkdir -m 0700 "$state/private"
    cp -a /etc/gdm3/daemon.conf "$state/private/daemon.conf.before"
    sha256sum /etc/gdm3/daemon.conf > "$state/private/daemon.conf.before.sha256"
    cat > "$state/private/rollback.sh" <<'ROLLBACK'
#!/bin/bash
set -euo pipefail
state=/run/t736-sbc-ac42-gi2
dropin=/run/systemd/user/org.gnome.Shell@x11.service.d/91-t736-sbc-ac42-gi2.conf
exec 9> "$state/private/lock"
flock -x 9
if [ -f "$state/private/rollback.complete" ]; then
    exit 0
fi
printf 'Restoring the baseline GNOME session configuration.\n'
rm -f -- "$dropin" "$state/1000.env"
cp -a "$state/private/daemon.conf.before" /etc/gdm3/daemon.conf
if [ -S /run/user/1000/bus ]; then
    timeout 10s runuser -u t736 -- env XDG_RUNTIME_DIR=/run/user/1000 \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
        systemctl --user daemon-reload || true
fi
timeout 45s systemctl restart display-manager.service
sha256sum -c "$state/private/daemon.conf.before.sha256"
date -u +%FT%TZ > "$state/private/rollback.complete"
ROLLBACK
    chmod 0700 "$state/private/rollback.sh"

    # Serialize publication with rollback, including a timer that fires early.
    exec 9> "$state/private/lock"
    flock -x 9
    # Arm a system-manager timer before the user-manager override exists.
    timeout 20s systemd-run --unit="$unit" --on-active="${seconds}s" \
        --timer-property=AccuracySec=1s \
        /bin/bash "$state/private/rollback.sh"
    timeout 10s systemctl is-active --quiet "$unit.timer"

    cat > "$state/1000.env" <<ENVIRONMENT
LD_LIBRARY_PATH=$libdir:$libdir/mutter-16:/opt/t736-img-ddk-24.2-x11/runtime-gles
GI_TYPELIB_PATH=$libdir/mutter-16:$libdir/girepository-1.0
COGL_DRIVER=gles2
COGL_EGL_X11_SBC_FEEDBACK=1
COGL_EGL_X11_SBC_TRACE=1
ENVIRONMENT
    chmod 0644 "$state/1000.env"
    mkdir -p "${dropin%/*}"
    cat > "$dropin" <<'DROPIN'
[Service]
ExecStart=
ExecStart=/opt/t736-mutter-sbc-48.7-ac42f10a/bin/gnome-shell-sbc-dispatch
# Only UID 1000 has a matching file. Other users keep their original environment.
EnvironmentFile=-/run/t736-sbc-ac42-gi2/%U.env
DROPIN
    chmod 0644 "$dropin"
    if [ -S /run/user/1000/bus ]; then
        timeout 10s runuser -u t736 -- env XDG_RUNTIME_DIR=/run/user/1000 \
            DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
            systemctl --user daemon-reload
    fi
    printf 'Armed %s.timer for %s seconds. Session has not been restarted.\n' "$unit" "$seconds"
    printf 'After the controlled GDM session start, restore daemon.conf.before immediately.\n'
    printf 'Keep the timer until library maps, SBC trace, GTK pacing and error deltas are checked.\n'
    ;;
rollback)
    [ -f "$state/private/rollback.sh" ]
    # Keep the timer armed until a synchronous, idempotent rollback succeeds.
    /bin/bash "$state/private/rollback.sh"
    systemctl stop "$unit.timer"
    ;;
*)
    printf 'Usage: %s arm [60..900 seconds] | rollback\n' "$0" >&2
    exit 2
    ;;
esac
