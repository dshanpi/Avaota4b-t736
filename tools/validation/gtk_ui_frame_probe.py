#!/usr/bin/python3
"""Short GTK4/libadwaita GSK UI scroll probe, not physical scanout timing.

Run a fresh process in the user's session, then compare another fresh process
with GSK_RENDERER=cairo. No GLArea, configuration edits or simulated input.
"""
import argparse
import json
import os
import resource
import statistics
import time

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Adw, Gdk, Gio, GLib, Gtk


def session_state():
    state = {}
    for key, name, path, interface, method, parameters in (
        ('locked', 'org.gnome.ScreenSaver', '/org/gnome/ScreenSaver',
         'org.gnome.ScreenSaver', 'GetActive', None),
        ('overview_active', 'org.gnome.Shell', '/org/gnome/Shell',
         'org.freedesktop.DBus.Properties', 'Get',
         GLib.Variant('(ss)', ('org.gnome.Shell', 'OverviewActive'))),
    ):
        try:
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            value = bus.call_sync(name, path, interface, method, parameters,
                                  None, Gio.DBusCallFlags.NO_AUTO_START,
                                  1500, None).unpack()[0]
            if isinstance(value, GLib.Variant):
                value = value.unpack()
            if not isinstance(value, bool):
                raise ValueError('Expected boolean session state')
            state[key] = value
        except (GLib.Error, ValueError) as error:
            state[key] = None
            state[key + '_error'] = str(error)
    return state


def unblocked(state):
    return state.get('locked') is False and state.get('overview_active') is False


def intervals(values):
    delta = sorted((b - a) / 1000 for a, b in zip(values, values[1:]) if b > a)
    return {'count': len(delta), 'median_ms': statistics.median(delta) if delta else None,
            'p95_ms': delta[min(len(delta) - 1, int(len(delta) * .95))] if delta else None,
            'over_33_5ms': sum(value > 33.5 for value in delta)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=10)
    args = parser.parse_args()
    if not 8 <= args.seconds <= 12:
        parser.error('seconds must be between 8 and 12')
    before = session_state()
    if not unblocked(before):
        print(json.dumps({'status': 'SKIPPED', 'session_before': before,
                          'reason': 'Locked/overview active or session state unknown'}))
        return 2
    launched, launch_cpu = time.monotonic(), time.process_time()
    state = {'mapped_at': None, 'cpu_at_map': None, 'renderer': None, 'finished': False}
    ticks, paints, scroll_values = [], [], []
    app = Adw.Application(application_id='local.t736.UiFrameProbe',
                          flags=Gio.ApplicationFlags.NON_UNIQUE)

    def finish(interrupted=False):
        if state['finished']:
            return False
        state['finished'] = True
        ended, cpu_ended = time.monotonic(), time.process_time()
        usage = resource.getrusage(resource.RUSAGE_SELF)
        after = session_state()
        maps = open('/proc/self/maps', encoding='utf-8').read().splitlines()
        libraries = sorted({line.split()[-1] for line in maps if '/' in line and any(
            name in line for name in ('libgtk-4', 'libadwaita', 'libEGL', 'libGLES',
                                     'libGL.', 'libpvr', 'swrast', 'libLLVM', 'libcairo'))})
        tick_stats, paint_stats = intervals(ticks), intervals(paints)
        mapped = state['mapped_at'] is not None
        moved = len(set(scroll_values)) > 1
        usable = (mapped and moved and state['renderer'] is not None and
                  not interrupted and unblocked(after) and paint_stats['count'] >= 120)
        sample_elapsed = ended - state['mapped_at'] if mapped else None
        print(json.dumps({
            'status': 'COMPLETE' if usable else 'UNUSABLE', 'timing_usable': usable,
            'workload': '24 rounded libadwaita cards with labels, buttons and switches; 240px/s scroll',
            'renderer_gtype': state['renderer'], 'requested_renderer': os.environ.get('GSK_RENDERER'),
            'environment': {key: os.environ.get(key) for key in
                            ('GDK_BACKEND', 'GDK_DISABLE', 'GSK_RENDERER', 'LD_LIBRARY_PATH')},
            'sample_seconds_requested': args.seconds, 'sample_elapsed_seconds': sample_elapsed,
            'mapped_seconds_after_launch': state['mapped_at'] - launched if mapped else None,
            'first_tick_seconds_after_launch': state.get('first_tick_at'),
            'cpu_seconds_before_map': state['cpu_at_map'] - launch_cpu if mapped else None,
            'cpu_seconds_sample': cpu_ended - state['cpu_at_map'] if mapped else None,
            'cpu_seconds_total': cpu_ended - launch_cpu,
            'rusage_user_seconds': usage.ru_utime, 'rusage_system_seconds': usage.ru_stime,
            'max_rss_kib': usage.ru_maxrss, 'tick_intervals': tick_stats,
            'after_paint_intervals': paint_stats, 'tick_count': len(ticks),
            'after_paint_count': len(paints), 'scroll_changed': moved,
            'libraries': libraries, 'session_before': before, 'session_after': after,
            'session_check_scope': 'start and end only', 'interrupted': interrupted,
            'physical_display_verified': False,
            'metric': 'GTK frame-clock tick and monotonic after-paint callback intervals; not scanout/presentation',
        }, indent=2), flush=True)
        app.quit()
        return False

    def activate(application):
        window = Adw.ApplicationWindow(application=application, title='T736 GSK UI scroll probe')
        window.set_default_size(820, 620)
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        header = Adw.HeaderBar()
        header.set_title_widget(Adw.WindowTitle(title='GSK UI probe', subtitle=f'{args.seconds:g} second card scrolling'))
        outer.append(header)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        for setter in ('set_margin_top', 'set_margin_bottom', 'set_margin_start', 'set_margin_end'):
            getattr(content, setter)(20)
        for index in range(24):
            card = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=14)
            card.add_css_class('card')
            card.add_css_class('probe-card')
            labels = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, hexpand=True)
            title = Gtk.Label(label=f'Settings card {index + 1:02}', xalign=0)
            title.add_css_class('heading')
            labels.append(title)
            labels.append(Gtk.Label(label='Native text, rounded background, shadow and controls', xalign=0))
            card.append(labels)
            button = Gtk.Button(label='Details', valign=Gtk.Align.CENTER)
            if index % 3 == 0:
                button.add_css_class('suggested-action')
            card.append(button)
            card.append(Gtk.Switch(active=bool(index % 2), valign=Gtk.Align.CENTER))
            content.append(card)
        css = Gtk.CssProvider()
        css.load_from_data(b'.probe-card { padding: 18px; border-radius: 18px; box-shadow: 0 3px 9px alpha(black,0.18); }')
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        scroll.set_child(content)
        outer.append(scroll)
        window.set_content(outer)

        def mapped(widget):
            if state['mapped_at'] is not None:
                return
            state['mapped_at'], state['cpu_at_map'] = time.monotonic(), time.process_time()
            renderer = window.get_renderer()
            state['renderer'] = renderer.__gtype__.name if renderer else None
            window.get_frame_clock().connect('after-paint', lambda clock: paints.append(time.monotonic_ns() // 1000))
            GLib.timeout_add(int(args.seconds * 1000), finish)

        def tick(widget, clock):
            if state['finished']:
                return False
            now = clock.get_frame_time()
            if not ticks:
                state['first_tick_at'] = time.monotonic() - launched
            ticks.append(now)
            adjustment = scroll.get_vadjustment()
            span = min(1000.0, max(0.0, adjustment.get_upper() - adjustment.get_page_size()))
            if span:
                phase = (now - ticks[0]) / 1_000_000 * 240 % (2 * span)
                value = span - abs(span - phase)
                adjustment.set_value(value)
                scroll_values.append(round(value, 3))
            return True

        window.connect('map', mapped)
        window.connect('close-request', lambda widget: (finish(True), False)[1])
        window.add_tick_callback(tick)
        window.present()
        # Startup watchdog; native-library stalls can still delay the main loop.
        GLib.timeout_add(int((args.seconds + 10) * 1000), lambda: finish(True))

    app.connect('activate', activate)
    return app.run([])


if __name__ == '__main__':
    raise SystemExit(main())
