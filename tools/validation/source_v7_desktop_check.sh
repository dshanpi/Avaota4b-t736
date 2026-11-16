#!/bin/bash
# Read-only runtime checks for the T736 source-v7 GNOME image.
# No service changes, package installation, raw input capture or secret output.
set -uo pipefail
export LC_ALL=C

usage() {
    cat <<'HELP'
Usage: bash source_v7_desktop_check.sh [--help]

Run on the booted board, preferably as root, and redirect stdout/stderr to an
evidence log outside this script. Expected kernel: 6.18.63-t736-hdmi-v14.
Exit status: 0 = checks passed; 1 = failures; 2 = incomplete (missing tools,
permissions, or an interactive desktop session). PASS does not prove that a
monitor displays pixels, that input works physically, or that a password works.

The script only queries system state. It does not install packages, alter boot
targets, start/stop services, log in a user, take screenshots, or read passwords,
shadow files or raw keyboard events. It does not print authentication cookies.
When available, glxinfo -B runs in an existing local graphical user's environment with Mesa's
shader disk cache disabled. On Wayland this tests Xwayland GLX, not native EGL.

Actual GNOME screenshot evidence:
  1. Log into GNOME on the HDMI monitor and open a harmless test window.
  2. Press Print Screen, select the full screen, and use GNOME's capture button.
  3. Collect the resulting PNG from that user's Pictures/Screenshots directory
     (or its localized XDG Pictures equivalent); record its SHA-256 and boot ID.
  4. Also record a monitor photo when proving the physical HDMI output. A DRM
     framebuffer dump or a screenshot alone does not prove the monitor signal.
Do not capture passwords or private windows. GNOME may require user interaction
for screenshot permission; do not substitute a fabricated image or bypass it.
For the configured Xorg session, x11-apps also supplies xwd. From that user's
GNOME terminal (with its existing DISPLAY/XAUTHORITY), an explicit capture is:
    xwd -root -silent -out "$HOME/t736-desktop.xwd"
Copy that file to the build host and convert it to PNG with an available image
converter; inspect the PNG to confirm it contains the actual desktop. This is
an optional separate action, never performed by this validation script. GNOME
48's org.gnome.Shell.Screenshot D-Bus interface may reject external callers.

Authentication validation is a separate interactive check: use the intended
normal account at GDM, enter its password privately, verify the user's desktop,
then lock and unlock it. Record only success/failure and the session's user/type.
An already active session, root-run runuser, or automatic login does not validate
the password. Do not put passwords in shell arguments, scripts, or evidence logs.
HELP
}

if [[ ${1-} == --help ]]; then
    usage
    exit 0
fi
if (( $# )); then
    usage >&2
    exit 2
fi

failures=0
incomplete=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failures=$((failures + 1)); }
skip() { printf 'SKIP %s\n' "$*"; incomplete=$((incomplete + 1)); }
info() { printf 'INFO %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Bound D-Bus and graphics queries when coreutils timeout is available.
query() {
    if have timeout; then
        timeout 20s "$@"
    else
        "$@"
    fi
}

printf 'SOURCE_V7_DESKTOP_CHECK_BEGIN\n'
date -u '+UTC %Y-%m-%dT%H:%M:%SZ'
kernel=$(uname -r)
info "kernel=$kernel uid=$EUID"
if [[ $kernel == 6.18.63-t736-hdmi-v14 ]]; then
    pass 'expected v14 kernel'
else
    fail 'expected kernel 6.18.63-t736-hdmi-v14'
fi
if [[ -r /proc/sys/kernel/random/boot_id ]]; then
    read -r boot_id < /proc/sys/kernel/random/boot_id
    info "boot_id=$boot_id"
fi
if [[ -r /proc/sys/kernel/tainted ]]; then
    read -r taint < /proc/sys/kernel/tainted
    if [[ $taint == 0 ]]; then
        pass 'kernel taint=0'
    else
        fail "kernel taint=$taint"
    fi
else
    skip 'kernel taint is not readable'
fi
if have findmnt; then
    query findmnt -n -o SOURCE,FSTYPE,OPTIONS / || skip 'root mount query failed'
fi

if have systemctl; then
    if manager=$(query systemctl show display-manager.service --no-pager \
            -p Id -p LoadState -p ActiveState -p SubState 2>/dev/null); then
        printf '%s\n' "$manager"
        if grep -Eq '^Id=gdm3?\.service$' <<< "$manager" &&
                grep -qx 'LoadState=loaded' <<< "$manager" &&
                grep -qx 'ActiveState=active' <<< "$manager"; then
            pass 'GDM display manager is active'
        else
            fail 'display-manager.service is not an active GDM service'
        fi
    else
        skip 'display manager state unavailable'
    fi
    if failed_units=$(query systemctl --failed --no-legend --plain --no-pager 2>/dev/null); then
        if [[ -z $failed_units ]]; then
            pass 'no failed system units'
        else
            fail 'failed system units present'
            printf '%s\n' "$failed_units"
        fi
    else
        skip 'failed system unit query unavailable'
    fi
else
    skip 'systemctl unavailable: GDM and failed units not checked'
fi

shopt -s nullglob
drm_cards=(/dev/dri/card[0-9]*)
render_nodes=(/dev/dri/renderD[0-9]*)
if (( ${#drm_cards[@]} )); then
    pass "DRM card nodes: ${drm_cards[*]}"
else
    fail 'no DRM card device nodes'
fi
if (( ${#render_nodes[@]} )); then
    info "DRM render nodes: ${render_nodes[*]}"
else
    info 'no render node; software rendering may be used (see glxinfo)'
fi
hdmi_active=0
for connector in /sys/class/drm/card*-HDMI-A-*; do
    status=unknown
    enabled=unknown
    [[ ! -r $connector/status ]] || read -r status < "$connector/status"
    [[ ! -r $connector/enabled ]] || read -r enabled < "$connector/enabled"
    info "${connector##*/} status=$status enabled=$enabled"
    if [[ $status == connected && $enabled == enabled ]]; then
        hdmi_active=1
    fi
    if [[ -r $connector/modes ]]; then
        printf 'MODES %s\n' "${connector##*/}"
        head -n 8 "$connector/modes"
    fi
done
if (( hdmi_active )); then
    pass 'connected and enabled HDMI connector'
else
    fail 'no connected and enabled HDMI connector'
fi
for framebuffer in /sys/class/graphics/fb*/name; do
    read -r fb_name < "$framebuffer"
    info "$framebuffer=$fb_name"
done
input_nodes=(/dev/input/event[0-9]*)
if (( ${#input_nodes[@]} )); then
    pass "input event node count=${#input_nodes[@]} (enumeration only)"
else
    fail 'no input event device nodes'
fi
if [[ -r /proc/bus/input/devices ]]; then
    # Names and handler capabilities only; never consume /dev/input/event*.
    grep -E '^[NH]: ' /proc/bus/input/devices || true
fi

session_id=
session_uid=
session_name=
session_type=
session_display=
session_leader=
if have loginctl; then
    if session_list=$(query loginctl list-sessions --no-legend --no-pager 2>/dev/null); then
        while read -r candidate _; do
            [[ -n $candidate ]] || continue
            if ! details=$(query loginctl show-session "$candidate" --no-pager \
                    -p Id -p User -p Name -p Type -p Class -p Active -p Remote \
                    -p State -p Leader -p Display 2>/dev/null); then
                skip "session $candidate disappeared or is inaccessible"
                continue
            fi
            printf 'SESSION\n%s\n' "$details"
            s_uid= s_name= s_type= s_class= s_active= s_remote= s_display= s_leader=
            while IFS='=' read -r key value; do
                case $key in
                    User) s_uid=$value ;; Name) s_name=$value ;;
                    Type) s_type=$value ;; Class) s_class=$value ;;
                    Active) s_active=$value ;; Remote) s_remote=$value ;;
                    Display) s_display=$value ;; Leader) s_leader=$value ;;
                esac
            done <<< "$details"
            if [[ -z $session_id && $s_class == user && $s_active == yes &&
                    $s_remote == no && $s_type =~ ^(x11|wayland)$ &&
                    $s_uid =~ ^[0-9]+$ && -n $s_name ]]; then
                session_id=$candidate
                session_uid=$s_uid
                session_name=$s_name
                session_type=$s_type
                session_display=$s_display
                session_leader=$s_leader
            fi
        done <<< "$session_list"
    else
        skip 'loginctl session enumeration failed'
    fi
else
    skip 'loginctl unavailable'
fi

if [[ -z $session_id ]]; then
    skip 'no active local user desktop session; greeter alone is insufficient'
else
    pass "active local desktop session=$session_id user=$session_name type=$session_type"
    shell_pids=
    if have pgrep; then
        shell_pids=$(pgrep -u "$session_uid" -x gnome-shell || true)
        if [[ -n $shell_pids ]]; then
            pass 'gnome-shell process exists for the desktop user'
        else
            fail 'no gnome-shell process for the desktop user'
        fi
    else
        skip 'pgrep unavailable: GNOME process not checked'
    fi

    # Read only allowlisted display selectors from a graphical process. Never
    # dump /proc/PID/environ or read the Xauthority file/cookie contents.
    glx_display=$session_display
    glx_authority=
    glx_mesa=()
    for pid in $shell_pids "$session_leader"; do
        [[ $pid =~ ^[0-9]+$ && -r /proc/$pid/environ ]] || continue
        p_display= p_authority= p_session=
        p_mesa=()
        while IFS= read -r -d '' entry; do
            case $entry in
                DISPLAY=*) p_display=${entry#DISPLAY=} ;;
                XAUTHORITY=*) p_authority=${entry#XAUTHORITY=} ;;
                XDG_SESSION_ID=*) p_session=${entry#XDG_SESSION_ID=} ;;
                LIBGL_ALWAYS_SOFTWARE=*|GALLIUM_DRIVER=*|LIBGL_DRI3_DISABLE=*)
                    p_mesa+=("$entry") ;;
            esac
        done < "/proc/$pid/environ"
        [[ -z $p_session || $p_session == "$session_id" ]] || continue
        [[ -z $session_display || $p_display == "$session_display" ]] || continue
        if [[ -n $p_display ]]; then
            glx_display=$p_display
            glx_authority=$p_authority
            glx_mesa=("${p_mesa[@]}")
            break
        fi
    done

    if ! have glxinfo; then
        skip 'glxinfo unavailable; no package installation attempted'
    elif ! have timeout; then
        skip 'timeout unavailable: avoiding an unbounded graphics query'
    elif [[ -z $glx_display ]]; then
        skip 'desktop DISPLAY unavailable; run glxinfo -B in its GNOME terminal'
    elif (( EUID != 0 && EUID != session_uid )); then
        skip 'run as root or the active desktop user to query its GLX context'
    elif (( EUID == 0 && session_uid != 0 )) && ! have runuser; then
        skip 'runuser unavailable: cannot query GLX as the desktop user'
    elif ! have getent; then
        skip 'getent unavailable: cannot resolve the desktop user home'
    else
        user_home=$(getent passwd "$session_uid" | cut -d: -f6)
        if [[ -z $user_home ]]; then
            skip 'desktop user home lookup failed'
        else
            glx_command=(env -i "HOME=$user_home" "USER=$session_name"
                "LOGNAME=$session_name" PATH=/usr/local/bin:/usr/bin:/bin
                LC_ALL=C "DISPLAY=$glx_display" "XDG_RUNTIME_DIR=/run/user/$session_uid"
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$session_uid/bus"
                MESA_SHADER_CACHE_DISABLE=true "${glx_mesa[@]}")
            [[ -z $glx_authority ]] || glx_command+=("XAUTHORITY=$glx_authority")
            glx_command+=("$(command -v glxinfo)" -B)
            if (( EUID == 0 && session_uid != 0 )); then
                glx_command=(runuser -u "$session_name" -- "${glx_command[@]}")
            fi
            info 'GLX query follows; llvmpipe/softpipe indicates software rendering'
            if timeout 20s "${glx_command[@]}"; then
                pass 'existing desktop GLX context responds'
            else
                fail 'desktop glxinfo failed or timed out; inspect output above'
            fi
        fi
    fi
fi

info 'Screenshot, physical HDMI/input and password validation remain separate interactive checks; see --help.'
printf 'SOURCE_V7_DESKTOP_CHECK_END failures=%s incomplete=%s\n' "$failures" "$incomplete"
if (( failures )); then
    exit 1
elif (( incomplete )); then
    exit 2
fi
exit 0
