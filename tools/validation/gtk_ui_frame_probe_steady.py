#!/usr/bin/python3
"""Warm a GTK4/libadwaita scroll workload, then measure its steady interval.

Default: 5 seconds of warmup followed by 10 seconds of measurement. The
24-card UI, window size and 240 px/s animation match gtk_ui_frame_probe.py.
No configuration changes or simulated input. Callback timing is not physical
scanout, presentation timing, or a guarantee that shader compilation has ended.
"""
import argparse
import json
import math
import os
import statistics
import time


def intervals(values):
    delta = sorted((b - a) / 1000 for a, b in zip(values, values[1:]) if b > a)
    return {'count': len(delta), 'median_ms': statistics.median(delta) if delta else None,
            'p95_ms': delta[min(len(delta) - 1, int(len(delta) * .95))] if delta else None,
            'over_33_5ms': sum(value > 33.5 for value in delta)}


class SteadySample:
    """Count callbacks and CPU in contiguous, actually observed time windows.

    A delayed main-loop timer produces a longer window, never invented CPU
    subdivisions. The final window closes at the same instant as the sample.
    """

    def __init__(self, started, cpu_started):
        self.started, self.cpu_started = started, cpu_started
        self.previous_time, self.previous_cpu = started, cpu_started
        self.previous_ticks = self.previous_paints = 0
        self.ticks, self.paints, self.scroll_values, self.windows = [], [], [], []

    def snapshot(self, ended, cpu_ended, final=False):
        if ended <= self.previous_time:
            return
        self.windows.append({
            'window_index': len(self.windows) + 1,
            'start_elapsed_seconds': self.previous_time - self.started,
            'end_elapsed_seconds': ended - self.started,
            'actual_elapsed_seconds': ended - self.previous_time,
            'tick_count': len(self.ticks) - self.previous_ticks,
            'after_paint_count': len(self.paints) - self.previous_paints,
            'process_cpu_seconds': cpu_ended - self.previous_cpu,
            'final_window': final,
        })
        self.previous_time, self.previous_cpu = ended, cpu_ended
        self.previous_ticks, self.previous_paints = len(self.ticks), len(self.paints)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--warmup-seconds', type=float, default=5)
    parser.add_argument('--seconds', type=float, default=10)
    args = parser.parse_args()
    if not 3 <= args.warmup_seconds <= 10:
        parser.error('warmup-seconds must be between 3 and 10')
    if not 8 <= args.seconds <= 12:
        parser.error('seconds must be between 8 and 12')

    import resource
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Adw', '1')
    from gi.repository import Adw, Gdk, Gio, GLib, Gtk

    def session_state():
        result = {}
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
                result[key] = value
            except (GLib.Error, ValueError) as error:
                result[key] = None
                result[key + '_error'] = str(error)
        return result

    def unblocked(value):
        return value.get('locked') is False and value.get('overview_active') is False

    before = session_state()
    if not unblocked(before):
        print(json.dumps({'status': 'SKIPPED', 'session_before': before,
                          'reason': 'Locked/overview active or session state unknown'}))
        return 2
    launched, launch_cpu = time.monotonic(), time.process_time()
    state = {'mapped_at': None, 'cpu_at_map': None, 'renderer': None,
             'finished': False, 'sample': None, 'session_after_warmup': None,
             'warmup_ticks': 0, 'warmup_paints': 0, 'first_frame_time': None}
    app = Adw.Application(application_id='local.t736.UiFrameProbeSteady',
                          flags=Gio.ApplicationFlags.NON_UNIQUE)

    def finish(interrupted=False):
        if state['finished']:
            return False
        state['finished'] = True
        ended, cpu_ended = time.monotonic(), time.process_time()
        sample = state['sample']
        if sample is not None:
            sample.snapshot(ended, cpu_ended, final=True)
        usage = resource.getrusage(resource.RUSAGE_SELF)
        after = session_state()
        with open('/proc/self/maps', encoding='utf-8') as source:
            maps = source.read().splitlines()
        libraries = sorted({line.split()[-1] for line in maps if '/' in line and any(
            name in line for name in ('libgtk-4', 'libadwaita', 'libEGL', 'libGLES',
                                     'libGL.', 'libpvr', 'swrast', 'libLLVM', 'libcairo'))})
        tick_stats = intervals(sample.ticks if sample else [])
        paint_stats = intervals(sample.paints if sample else [])
        mapped = state['mapped_at'] is not None
        moved = sample is not None and len(set(sample.scroll_values)) > 1
        elapsed = ended - sample.started if sample else None
        usable = (mapped and moved and state['renderer'] is not None and
                  not interrupted and unblocked(after) and
                  unblocked(state['session_after_warmup'] or {}) and
                  paint_stats['count'] >= 120 and elapsed >= args.seconds)
        state['exit_code'] = 0 if usable else 2
        print(json.dumps({
            'schema': 't736-gtk-ui-frame-steady-v1',
            'status': 'COMPLETE' if usable else 'UNUSABLE', 'timing_usable': usable,
            'workload': '24 rounded libadwaita cards with labels, buttons and switches; 240px/s scroll',
            'animation_phase': 'continuous from first tick; not reset after warmup',
            'renderer_gtype': state['renderer'], 'requested_renderer': os.environ.get('GSK_RENDERER'),
            'environment': {key: os.environ.get(key) for key in
                            ('GDK_BACKEND', 'GDK_DISABLE', 'GSK_RENDERER', 'LD_LIBRARY_PATH',
                             'LIBGL_DRIVERS_PATH')},
            'warmup_seconds_requested': args.warmup_seconds,
            'warmup_elapsed_seconds': sample.started - state['mapped_at'] if sample else None,
            'warmup_tick_count': state['warmup_ticks'],
            'warmup_after_paint_count': state['warmup_paints'],
            'sample_seconds_requested': args.seconds, 'sample_elapsed_seconds': elapsed,
            'mapped_seconds_after_launch': state['mapped_at'] - launched if mapped else None,
            'first_tick_seconds_after_launch': state.get('first_tick_at'),
            'measurement_start_seconds_after_launch': sample.started - launched if sample else None,
            'cpu_seconds_before_map': state['cpu_at_map'] - launch_cpu if mapped else None,
            'cpu_seconds_warmup': sample.cpu_started - state['cpu_at_map'] if sample else None,
            'cpu_seconds_sample': cpu_ended - sample.cpu_started if sample else None,
            'cpu_seconds_total': cpu_ended - launch_cpu,
            'rusage_user_seconds': usage.ru_utime, 'rusage_system_seconds': usage.ru_stime,
            'max_rss_kib': usage.ru_maxrss, 'tick_intervals': tick_stats,
            'after_paint_intervals': paint_stats,
            'tick_count': len(sample.ticks) if sample else 0,
            'after_paint_count': len(sample.paints) if sample else 0,
            'per_second': sample.windows if sample else [],
            'per_second_scope': 'nominal 1s timer windows; use actual_elapsed_seconds; a delayed callback is not subdivided',
            'cpu_scope': 'this process and its threads only; excludes compositor, Xorg, GPU and kernel workers',
            'warmup_scope': 'includes session check after warmup; shader completion is not observed',
            'scroll_changed': moved, 'libraries': libraries,
            'session_before': before, 'session_after_warmup': state['session_after_warmup'],
            'session_after': after, 'session_check_scope': 'start, end of warmup, and end only',
            'interrupted': interrupted, 'physical_display_verified': False,
            'metric': 'GTK frame-clock tick and monotonic after-paint callback intervals; not scanout/presentation',
        }, indent=2), flush=True)
        app.quit()
        return False

    def measure_second():
        if state['finished']:
            return False
        sample = state['sample']
        now, cpu_now = time.monotonic(), time.process_time()
        if now - sample.started >= args.seconds:
            return finish()
        sample.snapshot(now, cpu_now)
        # Align with elapsed whole seconds, without fabricating skipped windows.
        target = min(args.seconds, math.floor(now - sample.started) + 1)
        GLib.timeout_add(max(1, math.ceil((sample.started + target - time.monotonic()) * 1000)),
                         measure_second)
        return False

    def begin_measurement():
        if state['finished']:
            return False
        state['session_after_warmup'] = session_state()
        if not unblocked(state['session_after_warmup']):
            return finish(True)
        state['sample'] = SteadySample(time.monotonic(), time.process_time())
        GLib.timeout_add(1000, measure_second)
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

        def painted(clock):
            if state['finished']:
                return
            sample = state['sample']
            if sample is None:
                state['warmup_paints'] += 1
            else:
                sample.paints.append(time.monotonic_ns() // 1000)

        def mapped(widget):
            if state['mapped_at'] is not None:
                return
            state['mapped_at'], state['cpu_at_map'] = time.monotonic(), time.process_time()
            renderer = window.get_renderer()
            state['renderer'] = renderer.__gtype__.name if renderer else None
            window.get_frame_clock().connect('after-paint', painted)
            GLib.timeout_add(int(args.warmup_seconds * 1000), begin_measurement)

        def tick(widget, clock):
            if state['finished']:
                return False
            now = clock.get_frame_time()
            if state['first_frame_time'] is None:
                state['first_frame_time'] = now
                state['first_tick_at'] = time.monotonic() - launched
            sample = state['sample']
            if sample is None:
                state['warmup_ticks'] += 1
            else:
                sample.ticks.append(now)
            adjustment = scroll.get_vadjustment()
            span = min(1000.0, max(0.0, adjustment.get_upper() - adjustment.get_page_size()))
            if span:
                phase = (now - state['first_frame_time']) / 1_000_000 * 240 % (2 * span)
                value = span - abs(span - phase)
                adjustment.set_value(value)
                if sample is not None:
                    sample.scroll_values.append(round(value, 3))
            return True

        window.connect('map', mapped)
        window.connect('close-request', lambda widget: (finish(True), False)[1])
        window.add_tick_callback(tick)
        window.present()
        # Startup watchdog; a native-library stall can delay the main loop.
        GLib.timeout_add(int((args.warmup_seconds + args.seconds + 10) * 1000),
                         lambda: finish(True))

    app.connect('activate', activate)
    result = app.run([])
    return state.get('exit_code', result)


if __name__ == '__main__':
    raise SystemExit(main())
