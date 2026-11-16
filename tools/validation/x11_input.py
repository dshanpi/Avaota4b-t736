#!/usr/bin/env python3
"""Send one bounded XTEST action to an explicitly selected X11 display.

Requires nonempty DISPLAY and XAUTHORITY environment variables; no discovery,
window focusing, clipboard access, or text logging is performed. Successful
actions are silent. The caller must observe the display before choosing input.

Examples, with DISPLAY/XAUTHORITY supplied by the caller:
    python3 x11_input.py key Return
    python3 x11_input.py key Super_L
    printf '%s' t736 | python3 x11_input.py text
    python3 x11_input.py click 640 480
    python3 x11_input.py motion 640 480

motion moves the pointer only: no button/key events or window focusing. It can
trigger the application's normal hover behavior at the observed coordinates.

Text is read only from a pipe, must contain at most 256 printable ASCII
characters, and must not include a trailing newline. It uses the current
group-0 keyboard mapping, with no active modifiers, and refuses unsupported
characters before typing anything. Passwords must never be passed as arguments.
"""
import argparse
import ctypes as C
import ctypes.util
import os
from pathlib import Path
import sys


class InputError(Exception):
    pass


class XkbState(C.Structure):
    _fields_ = [
        ('group', C.c_ubyte), ('locked_group', C.c_ubyte),
        ('base_group', C.c_ushort), ('latched_group', C.c_ushort),
        ('mods', C.c_ubyte), ('base_mods', C.c_ubyte),
        ('latched_mods', C.c_ubyte), ('locked_mods', C.c_ubyte),
        ('compat_state', C.c_ubyte), ('grab_mods', C.c_ubyte),
        ('compat_grab_mods', C.c_ubyte), ('lookup_mods', C.c_ubyte),
        ('compat_lookup_mods', C.c_ubyte), ('ptr_buttons', C.c_ushort),
    ]


def bind(lib, name, result, *arguments):
    function = getattr(lib, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


def require(condition, message):
    if not condition:
        raise InputError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('text', help='Type up to 256 printable ASCII characters from stdin')
    keys = commands.add_parser('key', help='Press and release one X11 keysym, such as Return')
    keys.add_argument('name')
    clicks = commands.add_parser('click', help='Move to a screen coordinate and left-click')
    clicks.add_argument('x', type=int)
    clicks.add_argument('y', type=int)
    motions = commands.add_parser('motion', help='Move the pointer without clicking or focusing a window')
    motions.add_argument('x', type=int)
    motions.add_argument('y', type=int)
    args = parser.parse_args()
    display_name = os.environ.get('DISPLAY', '')
    authority = os.environ.get('XAUTHORITY', '')
    require(bool(display_name and authority), 'Explicit DISPLAY and XAUTHORITY are required')
    require(Path(authority).is_file() and os.access(authority, os.R_OK),
            'The selected XAUTHORITY file is not readable')
    text = ''
    if args.action == 'text':
        require(not sys.stdin.isatty(), 'Text must be supplied through stdin without terminal echo')
        raw = sys.stdin.buffer.read(257)
        require(0 < len(raw) <= 256 and all(32 <= b <= 126 for b in raw),
                'Text must be 1..256 printable ASCII bytes without newlines')
        text = raw.decode('ascii')
        del raw

    x11 = C.CDLL(C.util.find_library('X11') or 'libX11.so.6')
    xtst = C.CDLL(C.util.find_library('Xtst') or 'libXtst.so.6')
    ptr = C.c_void_p
    integer_pointer = C.POINTER(C.c_int)
    open_display = bind(x11, 'XOpenDisplay', ptr, C.c_char_p)
    close_display = bind(x11, 'XCloseDisplay', C.c_int, ptr)
    flush = bind(x11, 'XFlush', C.c_int, ptr)
    default_screen = bind(x11, 'XDefaultScreen', C.c_int, ptr)
    display_width = bind(x11, 'XDisplayWidth', C.c_int, ptr, C.c_int)
    display_height = bind(x11, 'XDisplayHeight', C.c_int, ptr, C.c_int)
    string_to_keysym = bind(x11, 'XStringToKeysym', C.c_ulong, C.c_char_p)
    keysym_to_code = bind(x11, 'XKeysymToKeycode', C.c_ubyte, ptr, C.c_ulong)
    code_to_keysym = bind(x11, 'XkbKeycodeToKeysym', C.c_ulong,
                         ptr, C.c_ubyte, C.c_uint, C.c_uint)
    keyboard_state = bind(x11, 'XkbGetState', C.c_int,
                          ptr, C.c_uint, C.POINTER(XkbState))
    query_xtest = bind(xtst, 'XTestQueryExtension', C.c_int, ptr,
                       integer_pointer, integer_pointer, integer_pointer, integer_pointer)
    fake_key = bind(xtst, 'XTestFakeKeyEvent', C.c_int, ptr, C.c_uint, C.c_int, C.c_ulong)
    fake_button = bind(xtst, 'XTestFakeButtonEvent', C.c_int, ptr, C.c_uint, C.c_int, C.c_ulong)
    fake_motion = bind(xtst, 'XTestFakeMotionEvent', C.c_int,
                       ptr, C.c_int, C.c_int, C.c_int, C.c_ulong)
    display = open_display(display_name.encode())
    require(bool(display), 'Unable to open the explicitly selected X11 display')
    try:
        event, error, major, minor = (C.c_int() for _ in range(4))
        require(query_xtest(display, C.byref(event), C.byref(error),
                            C.byref(major), C.byref(minor)), 'XTEST is unavailable')

        def key_event(code, pressed):
            require(fake_key(display, code, int(pressed), 0), 'XTEST key event failed')

        def tap(code):
            key_event(code, True)
            try:
                key_event(code, False)
            finally:
                # A duplicate release is harmless and prevents a stuck key on error.
                fake_key(display, code, False, 0)

        if args.action == 'key':
            require(len(args.name) <= 64 and args.name.isascii(), 'Invalid keysym name')
            symbol = string_to_keysym(args.name.encode('ascii'))
            code = keysym_to_code(display, symbol) if symbol else 0
            require(bool(code), 'The requested keysym has no keyboard mapping')
            tap(code)
        elif args.action == 'text':
            state = XkbState()
            require(keyboard_state(display, 0x100, C.byref(state)) == 0,
                    'Unable to read keyboard modifier state')
            require(state.group == 0 and state.mods == 0,
                    'Text requires keyboard group 0 with no active or locked modifiers')
            shift = keysym_to_code(display, string_to_keysym(b'Shift_L'))
            require(bool(shift), 'The keyboard has no left Shift mapping')
            sequence = []
            for char in text:
                symbol = ord(char)
                code = keysym_to_code(display, symbol)
                require(bool(code), 'Text contains a character without a keyboard mapping')
                plain = code_to_keysym(display, code, 0, 0)
                shifted = code_to_keysym(display, code, 0, 1)
                require(symbol in (plain, shifted), 'Text needs an unsupported keyboard level')
                sequence.append((code, symbol != plain))
            # Resolve every character before sending the first input event.
            text = ''
            for code, shifted in sequence:
                if shifted:
                    key_event(shift, True)
                try:
                    tap(code)
                finally:
                    if shifted:
                        fake_key(display, shift, False, 0)
        else:
            screen = default_screen(display)
            require(0 <= args.x < display_width(display, screen) and
                    0 <= args.y < display_height(display, screen),
                    'Pointer coordinates are outside the selected screen')
            require(fake_motion(display, screen, args.x, args.y, 0), 'XTEST pointer motion failed')
            if args.action == 'click':
                require(fake_button(display, 1, True, 0), 'XTEST button press failed')
                try:
                    require(fake_button(display, 1, False, 0), 'XTEST button release failed')
                finally:
                    fake_button(display, 1, False, 0)
        flush(display)
    finally:
        close_display(display)


if __name__ == '__main__':
    try:
        main()
    except (InputError, OSError, AttributeError, UnicodeError):
        # Never include input, passwords, command arguments, or authentication data.
        print('X11 input failed; check display authorization, keyboard state, and action.', file=sys.stderr)
        raise SystemExit(1)
