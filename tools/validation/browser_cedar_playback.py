#!/usr/bin/env python3
"""Measure only the explicitly launched private Firefox test instance."""
import argparse
import json
import os
from pathlib import Path
import socket
import time
from firefox_renderer_probe_lib import Marionette

ap = argparse.ArgumentParser()
ap.add_argument('--url', required=True)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--firefox', type=int, required=True)
ap.add_argument('--broker', type=int, required=True)
ap.add_argument('--port', type=int, default=2829)
args = ap.parse_args()
assert not args.output.exists()
assert args.url.startswith('http://127.0.0.1:8765/')

def cpu_snapshot():
    result, parents = {}, {}
    for directory in Path('/proc').iterdir():
        if not directory.name.isdigit(): continue
        try:
            stat = (directory/'stat').read_text().rsplit(')',1)[1].split()
            pid = int(directory.name)
            parents[pid] = int(stat[1])
            result[pid] = {'ticks': int(stat[11]) + int(stat[12]),
                           'comm': (directory/'comm').read_text().strip()}
        except (OSError, ValueError): pass
    selected = {args.firefox, args.broker}
    while True:
        more = {pid for pid, parent in parents.items() if parent in selected}
        if more <= selected: break
        selected |= more
    return {pid: value for pid, value in result.items() if pid in selected}

def irq_count():
    for line in Path('/proc/interrupts').read_text().splitlines():
        if 't736-cedar-wc' in line:
            numbers = []
            for word in line.split(':',1)[1].split():
                if not word.isdigit(): break
                numbers.append(int(word))
            return sum(numbers)
    raise RuntimeError('Decoder interrupt not found')

with socket.create_connection(('127.0.0.1', args.port), timeout=10) as sock:
    client = Marionette(sock, 20)
    client.command('WebDriver:NewSession', {'capabilities': {}})
    before, old_irq = cpu_snapshot(), irq_count()
    start = time.monotonic()
    client.command('WebDriver:Navigate', {'url': args.url})
    timeline = []
    result = {}
    while time.monotonic() - start < 30:
        reply = client.command('WebDriver:ExecuteScript', {
            'script': 'return window.report ? {...window.report(),samples:undefined,visibility:document.visibilityState} : {};',
            'args': []})
        result = reply.get('value', reply)
        timeline.append({'wall_seconds': time.monotonic() - start, **result})
        if result.get('ended') or result.get('error'): break
        time.sleep(.5)
    elapsed = time.monotonic() - start
    after, new_irq = cpu_snapshot(), irq_count()
    detail = client.command('WebDriver:ExecuteScript', {
        'script': 'return window.report ? window.report() : {};', 'args': []})
    client.command('WebDriver:DeleteSession')
ticks = os.sysconf('SC_CLK_TCK')
cpu = [{'pid': pid, 'comm': value['comm'],
        'percent_one_core': 100*(value['ticks']-before.get(pid,{}).get('ticks',0))/ticks/elapsed}
       for pid, value in after.items()]
record = {'schema': 1, 'url': args.url, 'wall_seconds': elapsed,
          'irq_before': old_irq, 'irq_after': new_irq, 'irq_delta': new_irq-old_irq,
          'cpu': cpu, 'timeline': timeline, 'final': detail.get('value',detail)}
args.output.write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps({'wall_seconds': elapsed, 'irq_delta': new_irq-old_irq,
                  'final': result, 'cpu': cpu}))
