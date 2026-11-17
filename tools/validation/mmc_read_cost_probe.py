#!/usr/bin/env python3
"""Read an existing regular file through O_DIRECT; record bounded CPU costs.

Exactly 64 repetitions of its first 8 MiB, using 1 MiB userspace reads.
No raw devices, storage writes, cache dropping, affinity or system changes.
This is a throughput observation without content validation; the device's
internal read cache can be warm. Run the separate integrity probe as well.
"""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import time


def workers():
    result = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdecimal():
            continue
        try:
            value = (path / 'stat').read_text()
            close = value.rindex(')')
            name = value[value.index('(') + 1:close]
            fields = value[close + 2:].split()
            if name.startswith(('kworker', 'mmc')):
                result[path.name] = {
                    'comm': name, 'start_ticks': int(fields[19]),
                    'cpu_ticks': int(fields[11]) + int(fields[12]),
                    'last_cpu': int(fields[36]),
                }
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return result


def counters():
    cpu = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:]))
    rows = [line.split() for line in Path('/proc/diskstats').read_text().splitlines()]
    disk = next(row for row in rows if row[2] == 'mmcblk0')
    return {'cpu_ticks': cpu, 'mmcblk0_diskstats': list(map(int, disk[3:]))}


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def observe(path):
    chunk = 1048576
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECT | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        initial = os.fstat(descriptor)
        if not stat.S_ISREG(initial.st_mode) or initial.st_size < 8 * chunk:
            raise ValueError('Input must be an existing regular file of at least 8 MiB')
        with mmap.mmap(-1, chunk) as buffer:
            # Fault in the destination before measuring file transfer costs.
            buffer[:] = bytes(chunk)
            tasks_before = workers()
            system_before = counters()
            cpu_start = time.process_time()
            started = time.monotonic()
            total = 0
            for _ in range(64):
                os.lseek(descriptor, 0, os.SEEK_SET)
                for _ in range(8):
                    received = os.readv(descriptor, [buffer])
                    if received != chunk:
                        raise RuntimeError('Short O_DIRECT read')
                    total += received
            seconds = time.monotonic() - started
            process_cpu = time.process_time() - cpu_start
            system_after = counters()
            tasks_after = workers()
        if identity(initial) != identity(os.fstat(descriptor)):
            raise RuntimeError('Input file identity, size or modification time changed')
        if identity(initial) != identity(os.stat(path, follow_symlinks=False)):
            raise RuntimeError('Input path changed during observation')
        hz = os.sysconf('SC_CLK_TCK')
        task_deltas = []
        for pid, item in tasks_after.items():
            old = tasks_before.get(pid)
            if not old or old['start_ticks'] != item['start_ticks']:
                continue
            delta = item['cpu_ticks'] - old['cpu_ticks']
            if delta > 0:
                task_deltas.append({
                    'pid': int(pid), 'comm': item['comm'],
                    'cpu_seconds': delta / hz,
                    'last_cpu_before': old['last_cpu'], 'last_cpu_after': item['last_cpu'],
                })
        return {
            'schema': 't736-mmc-read-cost-v1', 'status': 'OBSERVATION_COMPLETE',
            'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'kernel': os.uname().release,
            'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'file': str(path), 'file_bytes': initial.st_size,
            'bytes': total, 'repetitions': 64, 'bytes_per_repetition': 8 * chunk,
            'syscall_chunk_bytes': chunk, 'seconds': seconds,
            'MiB_s': total / chunk / seconds, 'process_cpu_seconds': process_cpu,
            'clock_ticks': hz,
            'system_cpu_tick_delta': [b - a for a, b in zip(system_before['cpu_ticks'], system_after['cpu_ticks'])],
            'mmcblk0_diskstats_delta': [b - a for a, b in zip(system_before['mmcblk0_diskstats'], system_after['mmcblk0_diskstats'])],
            'workers': sorted(task_deltas, key=lambda item: -item['cpu_seconds']),
            'raw_device_access': False, 'storage_writes': False, 'content_validation': False,
            'limits': [
                'Repeated prefix reads may hit warm device-internal caches; O_DIRECT bypasses the Linux page cache.',
                'System CPU counters include all processes; worker counters are tick-quantized and omit exited workers.',
                'Endpoint processor values are not CPU residency measurements.',
                'Disk request milliseconds include queue residence, not just controller service.',
                'Destination prefault differs from earlier ad hoc observations; compare frozen-probe runs separately.',
            ],
        }
    finally:
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(observe(args.file), indent=2))


if __name__ == '__main__':
    main()
