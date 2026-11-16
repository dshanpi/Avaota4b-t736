#!/usr/bin/env python3
"""Read-only X11 PID/window-map timing; a mapped window is not a painted frame.

Run inside the same X11 session as the target application. --start-monotonic
may be captured by the launcher immediately before Popen() on the same host.
This script sends no input, changes no properties and never closes the app.
"""
import argparse
import ctypes as C
import ctypes.util
import json
import os
import time


class WindowAttributes(C.Structure):
    _fields_ = [
        ('x', C.c_int), ('y', C.c_int), ('width', C.c_int), ('height', C.c_int),
        ('border_width', C.c_int), ('depth', C.c_int), ('visual', C.c_void_p),
        ('root', C.c_ulong), ('class_', C.c_int), ('bit_gravity', C.c_int),
        ('win_gravity', C.c_int), ('backing_store', C.c_int),
        ('backing_planes', C.c_ulong), ('backing_pixel', C.c_ulong),
        ('save_under', C.c_int), ('colormap', C.c_ulong),
        ('map_installed', C.c_int), ('map_state', C.c_int),
        ('all_event_masks', C.c_long), ('your_event_mask', C.c_long),
        ('do_not_propagate_mask', C.c_long), ('override_redirect', C.c_int),
        ('screen', C.c_void_p),
    ]


def observe(pid, timeout, interval, started):
    observed = time.monotonic()
    origin = observed if started is None else started
    if origin > observed:
        raise ValueError('start-monotonic is in the future')
    library = ctypes.util.find_library('X11')
    if not library:
        raise RuntimeError('libX11 was not found')
    x = C.CDLL(library)
    x.XOpenDisplay.argtypes = [C.c_char_p]
    x.XOpenDisplay.restype = C.c_void_p
    x.XDefaultRootWindow.argtypes = [C.c_void_p]
    x.XDefaultRootWindow.restype = C.c_ulong
    x.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    x.XInternAtom.restype = C.c_ulong
    x.XGetWindowProperty.argtypes = [
        C.c_void_p, C.c_ulong, C.c_ulong, C.c_long, C.c_long, C.c_int,
        C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_int),
        C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.c_void_p),
    ]
    x.XGetWindowProperty.restype = C.c_int
    x.XGetWindowAttributes.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(WindowAttributes)]
    x.XGetWindowAttributes.restype = C.c_int
    x.XFree.argtypes = [C.c_void_p]
    x.XFree.restype = C.c_int
    x.XCloseDisplay.argtypes = [C.c_void_p]
    x.XCloseDisplay.restype = C.c_int
    # A window can disappear between the property and attributes reads.
    # Keep the callback alive until after XCloseDisplay; ignore that race.
    error_handler_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)
    error_handler = error_handler_type(lambda display, event: 0)
    x.XSetErrorHandler.argtypes = [error_handler_type]
    x.XSetErrorHandler.restype = C.c_void_p
    x.XSetErrorHandler(error_handler)
    display = x.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot open the current X11 display')

    def words(window, atom, required_type, limit):
        if not atom:
            return []
        actual_type, count, remaining = C.c_ulong(), C.c_ulong(), C.c_ulong()
        actual_format = C.c_int()
        data = C.c_void_p()
        status = x.XGetWindowProperty(
            display, window, atom, 0, limit, 0, required_type,
            C.byref(actual_type), C.byref(actual_format), C.byref(count),
            C.byref(remaining), C.byref(data))
        try:
            if status or actual_type.value != required_type or actual_format.value != 32:
                return []
            if count.value > limit or (count.value and not data.value):
                return []
            # Xlib returns format32 property items as native unsigned longs.
            array = C.cast(data, C.POINTER(C.c_ulong))
            return [array[i] for i in range(count.value)]
        finally:
            if data.value:
                x.XFree(data)

    result = {
        'pid': pid, 'display': os.environ.get('DISPLAY'),
        'metric': 'First viewable X11 client window with matching _NET_WM_PID',
        'complete_render_verified': False, 'input_sent': False,
        'timeout_seconds': timeout, 'start_monotonic': origin,
        'observer_start_monotonic': observed,
        'start_origin': 'launcher' if started is not None else 'observer',
    }
    try:
        root = x.XDefaultRootWindow(display)
        client_atom = x.XInternAtom(display, b'_NET_CLIENT_LIST', 1)
        pid_atom = x.XInternAtom(display, b'_NET_WM_PID', 1)
        if not client_atom or not pid_atom:
            raise RuntimeError('The window manager does not expose the required EWMH atoms')
        while True:
            for window in words(root, client_atom, 33, 4096):  # XA_WINDOW
                if words(window, pid_atom, 6, 1) != [pid]:  # XA_CARDINAL
                    continue
                attributes = WindowAttributes()
                if x.XGetWindowAttributes(display, window, C.byref(attributes)) and attributes.map_state == 2:
                    found = time.monotonic()
                    return dict(result, status='mapped', window=hex(window),
                                elapsed_seconds=round(found - origin, 6),
                                observer_seconds=round(found - observed, 6),
                                width=attributes.width, height=attributes.height)
            now = time.monotonic()
            if now - observed >= timeout:
                return dict(result, status='timeout',
                            elapsed_seconds=round(now - origin, 6),
                            observer_seconds=round(now - observed, 6))
            time.sleep(min(interval, timeout - (now - observed)))
    finally:
        x.XCloseDisplay(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--timeout', type=float, default=30.0)
    parser.add_argument('--interval', type=float, default=0.05)
    parser.add_argument('--start-monotonic', type=float)
    args = parser.parse_args()
    if args.pid <= 0 or not 0 < args.timeout <= 30 or not 0.01 <= args.interval <= 1:
        parser.error('PID must be positive, timeout in (0,30], and interval in [0.01,1]')
    try:
        result = observe(args.pid, args.timeout, args.interval, args.start_monotonic)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({'status': 'error', 'pid': args.pid, 'error': str(error),
                          'complete_render_verified': False}))
        raise SystemExit(2)
    print(json.dumps(result))
    raise SystemExit(0 if result['status'] == 'mapped' else 1)


if __name__ == '__main__':
    main()
