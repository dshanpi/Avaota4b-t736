#!/usr/bin/python3
"""Bounded GTK4 GLArea frame-clock probe; not a physical-display FPS test."""
import ctypes
import json
import os
import statistics
import time
import gi

gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, GLib, Gio

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def session_state():
    state = {}
    for name, destination, path, interface, method, parameters in (
        ('locked', 'org.gnome.ScreenSaver', '/org/gnome/ScreenSaver',
         'org.gnome.ScreenSaver', 'GetActive', None),
        ('overview_active', 'org.gnome.Shell', '/org/gnome/Shell',
         'org.freedesktop.DBus.Properties', 'Get',
         GLib.Variant('(ss)', ('org.gnome.Shell', 'OverviewActive'))),
    ):
        try:
            value = bus.call_sync(destination, path, interface, method, parameters,
                                  None, Gio.DBusCallFlags.NO_AUTO_START, 3000,
                                  None).unpack()[0]
            if isinstance(value, GLib.Variant):
                value = value.unpack()
            if not isinstance(value, bool):
                raise ValueError('Expected a boolean session state')
            state[name] = value
        except (GLib.Error, ValueError) as error:
            state[name] = None
            state[name + '_error'] = str(error)
    return state


def session_blockers(state):
    reasons = []
    for name, reason in (('locked', 'screen is locked/blanked'),
                         ('overview_active', 'GNOME overview is active')):
        if state[name] is True:
            reasons.append(reason)
        elif state[name] is None:
            reasons.append('cannot determine ' + name)
    return reasons


before = session_state()
blockers = session_blockers(before)
if blockers:
    print(json.dumps({'status': 'SKIPPED', 'reasons': blockers,
                      'session_before': before, 'session_after': None}), flush=True)
    raise SystemExit('SKIPPED: ' + '; '.join(blockers) + '; frame timing would be invalid')

gl = ctypes.CDLL('libGLESv2.so.2')
gl.glClearColor.argtypes = [ctypes.c_float] * 4
gl.glClear.argtypes = [ctypes.c_uint]
gl.glGetString.argtypes = [ctypes.c_uint]
gl.glGetString.restype = ctypes.c_char_p
ticks = []
renders = 0
identity = {}
started = time.monotonic()


def render(area, context):
    global renders
    if not identity:
        for label, token in [('vendor', 0x1f00), ('renderer', 0x1f01), ('version', 0x1f02)]:
            value = gl.glGetString(token)
            identity[label] = value.decode() if value else None
        print(json.dumps({'gl': identity}), flush=True)
    phase = (time.monotonic() - started) % 2 / 2
    gl.glClearColor(0.10, 0.22 + phase * 0.3, 0.40, 1.0)
    gl.glClear(0x4000)
    renders += 1
    return True


def tick(area, frame_clock):
    ticks.append(frame_clock.get_frame_time())
    area.queue_render()
    return True


def finish():
    finished = time.monotonic()
    after = session_state()
    blockers = session_blockers(after)
    intervals = sorted((b-a)/1000 for a, b in zip(ticks, ticks[1:]) if b > a)
    maps = open('/proc/self/maps').read()
    libs = sorted(set(line.split()[-1] for line in maps.splitlines()
                      if any(name in line for name in ['libEGL', 'libGLES', 'swrast', 'libpvr'])))
    result = {'elapsed_seconds': round(finished-started, 3),
              'tick_count': len(ticks), 'render_count': renders, 'gl': identity,
              'libraries': libs, 'physical_display_verified': False,
              'session_before': before, 'session_after': after,
              'session_check_scope': 'start and end only',
              'metric': 'GTK frame-clock intervals, not scanout/presentation timestamps'}
    result['enough_samples_for_timing'] = len(intervals) >= 120
    result['timing_usable'] = result['enough_samples_for_timing'] and not blockers
    if blockers:
        result.update(status='SKIPPED', reasons=blockers)
    if intervals:
        result.update(interval_ms_median=statistics.median(intervals),
                      interval_ms_p95=intervals[min(len(intervals)-1, int(len(intervals)*0.95))],
                      intervals_over_33ms=sum(x > 33.5 for x in intervals),
                      interval_count=len(intervals))
    print(json.dumps(result, indent=2), flush=True)
    app.quit()
    return False


def activate(application):
    window = Gtk.ApplicationWindow(application=application, title='T736 GTK hardware frame probe')
    window.set_default_size(800, 600)
    area = Gtk.GLArea()
    area.set_use_es(True)
    area.set_required_version(3, 0)
    area.connect('render', render)
    area.add_tick_callback(tick)
    window.set_child(area)
    window.present()
    application.inhibit(window, Gtk.ApplicationInhibitFlags.IDLE, '12 second hardware frame test')
    GLib.timeout_add_seconds(12, finish)


app = Gtk.Application(application_id='local.t736.FrameProbe')
app.connect('activate', activate)
raise SystemExit(app.run([]))
