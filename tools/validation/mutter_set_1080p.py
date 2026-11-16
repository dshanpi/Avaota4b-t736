#!/usr/bin/env python3
"""Select current-session 1080p60 through Mutter; optionally save monitors.xml.

No persistent-method confirmation dialog: verify with method 0, apply with
method 1, observe stable state, then atomically save the matching user's XML.
Default is read-only. This fixed-board helper refuses multiple monitors.
"""
import argparse
import json
import math
import os
from pathlib import Path
import stat
import tempfile
import time
import xml.etree.ElementTree as ET

SERVICE = 'org.gnome.Mutter.DisplayConfig'
OBJECT = '/org/gnome/Mutter/DisplayConfig'
APPLY_SIGNATURE = '(uua(iiduba(ssa{sv}))a{sv})'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def select_config(state, connector='HDMI-1'):
    serial, monitors, logical, properties = state
    require(len(monitors) == 1, 'Expected exactly one connected physical monitor; refusing to disable other outputs')
    spec, modes, monitor_properties = monitors[0]
    require(spec[0] == connector, 'Connected output differs from requested connector')
    require(len(logical) == 1 and len(logical[0][5]) == 1
            and tuple(logical[0][5][0]) == tuple(spec), 'Expected one active logical monitor matching the output')
    require(logical[0][3] == 0, 'Rotated monitor needs a separate configuration review')
    choices = [mode for mode in modes if mode[1:3] == (1920, 1080)
               or list(mode[1:3]) == [1920, 1080]]
    choices = [mode for mode in choices if abs(mode[3] - 60.0) < 0.01
               and not mode[6].get('is-interlaced', False)
               and mode[6].get('refresh-rate-mode', 'fixed') == 'fixed'
               and any(math.isclose(scale, 1.0) for scale in mode[5])]
    require(choices, 'No fixed, progressive 1920x1080 near 60.000 Hz with scale 1 in current Mutter modes')
    mode = min(choices, key=lambda item: (abs(item[3] - 60.0), not item[6].get('is-preferred', False), item[0]))
    layout = properties.get('layout-mode', 1)
    require(layout in (1, 2), 'Unknown Mutter layout mode')
    require(monitor_properties.get('color-mode', 0) == 0, 'Non-default color mode needs a separate review')
    require(not monitor_properties.get('is-underscanning', False), 'Underscanning needs a separate review')
    return {'serial': serial, 'spec': list(spec), 'mode_id': mode[0],
            'width': 1920, 'height': 1080, 'refresh': mode[3], 'scale': 1.0,
            'transform': 0, 'x': 0, 'y': 0, 'primary': True,
            'layout_mode': layout, 'layout_name': 'physical' if layout == 2 else 'logical'}


def state_matches(state, plan):
    _serial, monitors, logical, properties = state
    if len(monitors) != 1 or len(logical) != 1:
        return False
    spec, modes, _props = monitors[0]
    current = [mode for mode in modes if mode[6].get('is-current', False)]
    row = logical[0]
    return (list(spec) == plan['spec'] and len(current) == 1 and current[0][0] == plan['mode_id']
            and tuple(row[:2]) == (0, 0) and math.isclose(row[2], 1.0) and row[3] == 0 and row[4]
            and len(row[5]) == 1 and list(row[5][0]) == plan['spec']
            and properties.get('layout-mode', 1) == plan['layout_mode'])


def monitors_xml(original, plan):
    root = ET.fromstring(original) if original else ET.Element('monitors', {'version': '2'})
    require(root.tag == 'monitors' and root.get('version') == '2', 'Existing monitors.xml has an unsupported format')
    names = ('connector', 'vendor', 'product', 'serial')
    for config in list(root.findall('configuration')):
        specs = [[item.findtext(name, '') for name in names] for item in config.findall('.//monitorspec')]
        if specs == [plan['spec']]:
            root.remove(config)
    config = ET.SubElement(root, 'configuration')
    ET.SubElement(config, 'layoutmode').text = plan['layout_name']
    logical = ET.SubElement(config, 'logicalmonitor')
    for name, value in [('x', 0), ('y', 0), ('scale', 1), ('primary', 'yes')]:
        ET.SubElement(logical, name).text = str(value)
    transform = ET.SubElement(logical, 'transform')
    ET.SubElement(transform, 'rotation').text = 'normal'
    ET.SubElement(transform, 'flipped').text = 'no'
    monitor = ET.SubElement(logical, 'monitor')
    spec = ET.SubElement(monitor, 'monitorspec')
    for name, value in zip(names, plan['spec']):
        ET.SubElement(spec, name).text = value
    mode = ET.SubElement(monitor, 'mode')
    for name, value in [('width', 1920), ('height', 1080), ('rate', f"{plan['refresh']:.3f}")]:
        ET.SubElement(mode, name).text = str(value)
    ET.indent(root, space='  ')
    return ET.tostring(root, encoding='utf-8', xml_declaration=False) + b'\n'


def save_xml(path, original, data):
    require(not path.is_symlink(), 'Refusing a symlink monitors.xml')
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    require(path.parent.stat().st_uid == os.geteuid(), 'Run as the session user that owns this config directory')
    current = path.read_bytes() if path.exists() else None
    require(current == original, 'monitors.xml changed during verification; refusing to replace it')
    backup = None
    mode = 0o600
    if path.exists():
        require(path.stat().st_uid == os.geteuid(), 'Existing monitors.xml belongs to another user')
        mode = stat.S_IMODE(path.stat().st_mode)
        backup = path.with_name(path.name + '.pre-t736-1080p-' + time.strftime('%Y%m%dT%H%M%S') + '-' + str(os.getpid()))
        with backup.open('xb') as stream:
            stream.write(original)
        backup.chmod(mode)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return str(backup) if backup else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--connector', default='HDMI-1')
    parser.add_argument('--bus-address', help='Optional explicit session bus address; otherwise use the session environment')
    parser.add_argument('--apply', action='store_true', help='Apply with method 1 only; do not save XML')
    parser.add_argument('--persist', action='store_true', help='Apply with method 1, verify stable state, then save this user\'s XML')
    parser.add_argument('--config', type=Path, default=Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config'))) / 'monitors.xml')
    parser.add_argument('--settle-seconds', type=int, choices=range(1, 61), default=25, metavar='1..60')
    args = parser.parse_args()
    from gi.repository import Gio, GLib
    if args.bus_address:
        connection = Gio.DBusConnection.new_for_address_sync(args.bus_address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    else:
        connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(method, parameters=None):
        return connection.call_sync(SERVICE, OBJECT, SERVICE, method, parameters, None,
                                    Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()

    state = call('GetCurrentState')
    plan = select_config(state, args.connector)
    report = {'plan': plan, 'currently_matches': state_matches(state, plan),
              'apply_method': 1, 'global_dbus_properties': {}, 'persistent_file_written': False,
              'session_restart_verified': False, 'physical_screen_verified': False}
    if not (args.apply or args.persist):
        print(json.dumps(report, indent=2), flush=True)
        return
    config = args.config.expanduser().absolute()
    original = None
    data = None
    if args.persist:
        require(not config.is_symlink(), 'Refusing a symlink monitors.xml')
        original = config.read_bytes() if config.exists() else None
        data = monitors_xml(original, plan)
        if config.parent.exists():
            require(config.parent.stat().st_uid == os.geteuid(), 'Use the session user for monitors.xml ownership')
    logical = [(0, 0, 1.0, 0, True, [(plan['spec'][0], plan['mode_id'], {})])]
    # Xorg cannot set layout-mode even though GetCurrentState reports 2.
    # Preserve the backend's current layout mode: pass an empty property dict.
    mode_applied = not report['currently_matches']
    for method in ((0, 1) if mode_applied else (0,)):
        call('ApplyMonitorsConfig', GLib.Variant(APPLY_SIGNATURE, (plan['serial'], method, logical, {})))
    deadline = time.monotonic() + args.settle_seconds
    checks = 0
    while True:
        state = call('GetCurrentState')
        require(state_matches(state, plan), 'Mutter did not retain the selected mode; XML was not changed')
        checks += 1
        if time.monotonic() >= deadline:
            break
        time.sleep(min(1.0, max(0, deadline - time.monotonic())))
    if args.persist:
        report['xml_backup'] = save_xml(config, original, data)
        require(config.read_bytes() == data, 'monitors.xml readback differs')
        report['config'] = str(config)
        report['persistent_file_written'] = True
    report.update(stable_samples=checks, stable_seconds=args.settle_seconds, final_serial=state[0],
                  current_mode_verified=True, mode_applied=mode_applied)
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
