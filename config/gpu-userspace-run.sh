#!/bin/sh
set -eu
. /opt/t736-img-ddk-24.2-x11/session.env
python3 "$T736_IMG_ROOT/bin/gpu-runtime-view" --verify >/dev/null
case "${1:-}" in
    probe-x11) shift; exec "$T736_IMG_ROOT/bin/gpu-probe" x11 "$@" ;;
    probe-gbm) shift; exec "$T736_IMG_ROOT/bin/gpu-probe" gbm "$@" ;;
    gnome-session) shift; exec /usr/bin/gnome-session --session=gnome "$@" ;;
    *) printf 'Usage: t736-img-run probe-x11 | probe-gbm /dev/dri/renderD<N> | gnome-session\n' >&2; exit 2 ;;
esac
