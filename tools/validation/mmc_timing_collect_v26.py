#!/usr/bin/env python3
"""Read v26 timing snapshots around the unchanged, pinned read-cost probe.

No device writes, counter resets, cache dropping, affinity, or tuning changes.
The probe reads an existing ordinary file on mmcblk0p4. Segment maxima are
lifetime values; endpoint CPU observations do not establish residency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

PROBE_SHA256 = 'c5b7ae1169bdab4cbc690d13f01b7e1b6f627f1cbe523fdbcbad96829a611946'
RELEASE = '6.18.63-t736-storage-v26'
STATS = Path('/sys/kernel/debug/t736_mmc_timing/stats')
SEGMENTS = ('dispatch', 'worker_service_before_done', 'dma_start', 'pre_reset',
            'write_copy', 'command_wait', 'busy_wait', 'post_reset', 'read_copy',
            'completion_callback', 'worker_total_before_merge')
DIRECTIONS = ('read', 'write')
REQUEST_FIELDS = ('attempted', 'success', 'failed', 'requested_bytes',
                  'completed_bytes', 'poll_iterations', 'poll_sleep_attempts')
BIN_FIELDS = ('le4096', 'le16384', 'le65536', 'gt65536')
SEGMENT_FIELDS = ('calls', 'total_ns', 'max_ns', 'different_endpoints', 'different_endpoint_ns')
CPU_FIELDS = ('starts', 'ends', 'same_endpoints', 'same_endpoint_ns')


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def numbers(items, fields):
    require(set(items) == set(fields), 'Unexpected numeric fields: ' + repr(items))
    require(all(re.fullmatch(r'[0-9]+', item) for item in items.values()), 'Invalid unsigned counter')
    return {key: int(items[key]) for key in fields}


def parse_snapshot(raw):
    rows = raw.splitlines()
    require(rows and len(raw) < 1048576, 'Empty or oversized timing snapshot')
    header = dict(token.split('=', 1) for token in rows[0].split())
    require(set(header) == {'schema', 'snapshot_ns', 'enabled'} and
            header['schema'] == '1' and header['enabled'] == '1', 'Unsupported/disabled timing schema')
    result = {'snapshot_ns': int(header['snapshot_ns']), 'metadata': {}, 'directions': {}}
    for name in DIRECTIONS:
        result['directions'][name] = {'segments': {}}
    seen = set()
    for line in rows[1:]:
        kind, separator, body = line.partition(' ')
        if not separator:
            key, value = line.split('=', 1)
            require(key in ('scope', 'units', 'nested', 'limits', 'snapshot', 'cpu_bucket_8') and key not in seen,
                    'Unexpected/duplicate metadata')
            result['metadata'][key] = value
            seen.add(key)
            continue
        if kind == 'units=wall_ns':
            require('units' not in seen, 'Duplicate units metadata')
            result['metadata']['units'] = line[len('units='):]
            seen.add('units')
            continue
        tokens = body.split()
        pairs = [token.split('=', 1) for token in tokens]
        require(all(len(pair) == 2 for pair in pairs) and len({pair[0] for pair in pairs}) == len(pairs),
                'Malformed/duplicate fields')
        fields = dict(pairs)
        if kind == 'samples':
            require('samples' not in seen, 'Duplicate samples')
            result['samples'] = numbers(fields, ('complete', 'incomplete', 'dropped_events'))
            seen.add('samples')
            continue
        direction = fields.pop('direction', None)
        require(direction in DIRECTIONS, 'Unknown direction')
        target = result['directions'][direction]
        if kind in ('requests', 'size_bins'):
            require(kind not in target, 'Duplicate direction totals')
            target[kind] = numbers(fields, REQUEST_FIELDS if kind == 'requests' else BIN_FIELDS)
        elif kind == 'segment':
            name = fields.pop('name', None)
            require(name in SEGMENTS and name not in target['segments'], 'Unknown/duplicate segment')
            target['segments'][name] = {**numbers(fields, SEGMENT_FIELDS), 'cpu_endpoints': {}}
        elif kind == 'cpu_endpoints':
            name = fields.pop('segment', None)
            bucket = fields.pop('cpu_bucket', None)
            require(name in target['segments'] and bucket in tuple(map(str, range(9))), 'Invalid CPU row')
            cpus = target['segments'][name]['cpu_endpoints']
            require(bucket not in cpus, 'Duplicate CPU bucket')
            cpus[bucket] = numbers(fields, CPU_FIELDS)
        else:
            raise RuntimeError('Unknown timing row: ' + kind)
    require(seen == {'scope', 'units', 'nested', 'limits', 'snapshot', 'samples', 'cpu_bucket_8'}, 'Missing timing metadata')
    require(result['samples']['complete'] == sum(
        item.get('requests', {}).get('attempted', -1) for item in result['directions'].values()),
        'Completed sample/request count mismatch')
    for direction in result['directions'].values():
        require(set(direction) == {'requests', 'size_bins', 'segments'} and
                set(direction['segments']) == set(SEGMENTS), 'Incomplete direction/segment rows')
        request = direction['requests']
        require(request['success'] + request['failed'] == request['attempted'] == sum(direction['size_bins'].values()),
                'Request result/size totals mismatch')
        require(request['completed_bytes'] <= request['requested_bytes'], 'Completed more bytes than requested')
        require(request['poll_iterations'] - request['poll_sleep_attempts'] ==
                direction['segments']['command_wait']['calls'], 'Poll iteration/sleep totals mismatch')
        for segment in direction['segments'].values():
            require(set(segment['cpu_endpoints']) == set(map(str, range(9))), 'Missing CPU bucket')
            cpus = segment['cpu_endpoints'].values()
            require(sum(item['starts'] for item in cpus) == sum(item['ends'] for item in cpus) == segment['calls'],
                    'CPU endpoint counts mismatch')
            require(sum(item['same_endpoints'] for item in cpus) + segment['different_endpoints'] == segment['calls'],
                    'Same/different endpoint counts mismatch')
            require(sum(item['same_endpoint_ns'] for item in cpus) + segment['different_endpoint_ns'] == segment['total_ns'],
                    'CPU endpoint nanoseconds mismatch')
            require(segment['max_ns'] <= segment['total_ns'], 'Invalid lifetime maximum')
    return result


def snapshot():
    with STATS.open('rb') as stream:
        data = stream.read(1048576)
    raw = data.decode('ascii')
    return {'raw': raw, 'sha256': hashlib.sha256(data).hexdigest(), 'parsed': parse_snapshot(raw)}


def difference(before, after):
    require(before['metadata'] == after['metadata'], 'Timing metadata changed')
    require(after['snapshot_ns'] > before['snapshot_ns'], 'Snapshot clock did not advance')

    def subtract(first, second):
        result = {}
        require(set(first) == set(second), 'Counter schema changed')
        for key in first:
            if isinstance(first[key], dict):
                result[key] = subtract(first[key], second[key])
            elif key == 'max_ns':
                require(second[key] >= first[key], 'Lifetime maximum decreased')
                result['lifetime_max_ns_before'] = first[key]
                result['lifetime_max_ns_after'] = second[key]
            else:
                require(second[key] >= first[key], 'Counter decreased: ' + key)
                result[key] = second[key] - first[key]
        return result

    result = {'snapshot_interval_ns': after['snapshot_ns'] - before['snapshot_ns'],
              'samples': subtract(before['samples'], after['samples']),
              'directions': subtract(before['directions'], after['directions'])}
    for direction in result['directions'].values():
        for segment in direction['segments'].values():
            segment['mean_wall_ns_per_call'] = segment['total_ns'] / segment['calls'] if segment['calls'] else None
    return result


def observe(path, probe):
    require(os.uname().release == RELEASE, 'Expected the reviewed v26 kernel')
    require(digest(probe) == PROBE_SHA256, 'Read-cost probe differs from frozen baseline')
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and (os.major(info.st_dev), os.minor(info.st_dev)) == (179, 4),
            'Expected an existing regular file on mmcblk0p4')
    boot_id = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    before = snapshot()
    command = [sys.executable, str(probe), '--file', str(path)]
    process = subprocess.run(command, text=True, capture_output=True, timeout=60)
    first_after = snapshot()
    # The worker aggregates after mmc_request_done; a read syscall can return
    # first. Observe bounded stability without resetting or draining the device.
    after = first_after
    stable = False
    stability_snapshots = []
    for _ in range(5):
        time.sleep(0.01)
        following = snapshot()
        stability_snapshots.append({'snapshot_ns': following['parsed']['snapshot_ns'],
                                    'samples': following['parsed']['samples']})
        if following['parsed']['samples'] == after['parsed']['samples']:
            after = following
            stable = True
            break
        after = following
    require(boot_id == Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'Boot changed')
    require(digest(probe) == PROBE_SHA256, 'Read-cost probe changed during observation')
    require(process.returncode == 0, 'Frozen probe failed: ' + process.stderr[-4096:])
    observed = json.loads(process.stdout)
    require(observed.get('status') == 'OBSERVATION_COMPLETE' and observed.get('script_sha256') == PROBE_SHA256
            and observed.get('kernel') == RELEASE and observed.get('boot_id') == boot_id
            and observed.get('bytes') == 536870912, 'Unexpected frozen probe result')
    final_info = path.lstat()
    identity = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns)
    require(identity(info) == identity(final_info), 'Target identity changed across collection')
    delta = difference(before['parsed'], after['parsed'])
    reads = delta['directions']['read']['requests']
    lower_bound = (reads['attempted'] >= 8192 and reads['success'] >= 8192
                   and reads['requested_bytes'] >= 536870912 and reads['completed_bytes'] >= 536870912)
    complete = (delta['samples']['incomplete'] == 0 and delta['samples']['dropped_events'] == 0
                and lower_bound)
    return {'schema': 't736-mmc-timing-collection-v1',
            'status': 'OBSERVATION_COMPLETE' if complete else 'INCOMPLETE_TIMING_SAMPLES',
            'collector_sha256': digest(__file__), 'kernel': RELEASE, 'boot_id': boot_id,
            'preflight_inode': info.st_ino, 'preflight_identity_matches_after': True,
            'target_device_major_minor': '179:4',
            'probe': observed, 'probe_stderr': process.stderr,
            'before': {'raw': before['raw'], 'sha256': before['sha256']},
            'first_after': {'raw': first_after['raw'], 'sha256': first_after['sha256']},
            'after': {'raw': after['raw'], 'sha256': after['sha256']},
            'after_sample_counts_observed_stable': stable, 'stability_observations': stability_snapshots,
            'delta': delta, 'expected_probe_data_requests_at_64KiB': 8192,
            'timing_read_count_and_byte_lower_bounds_satisfied': lower_bound,
            'limits': [
                'Snapshots contain only fully merged requests, including background ordinary I/O. Stability observations are not a hardware drain or proof that no request is outstanding.',
                'Read-count/byte lower bounds reject obvious missing coverage; background reads can mask a smaller omission. Preflight inode is an outer observation, not a hash embedded by the frozen probe.',
                'Snapshot, syscall, system CPU, worker CPU and diskstats windows have different endpoints. Compare counts and retain background I/O rather than forcing equality.',
                'dma_start includes pre_reset/write_copy and v26 sync_device; post_reset includes v26 sync_cpu. worker_total includes service/callback. Do not add nested segment totals; v25 reset-window costs are not directly comparable without accounting for added cache synchronization.',
                'Wall durations include interrupts/scheduling. Same CPU endpoints do not prove residency. dispatch crosses submitter/worker contexts, so differing endpoints indicate a handoff. Only within one worker segment do differing endpoints imply at least one migration.',
                'max_ns is a lifetime maximum. Only before/after maxima are reported; their difference is not the interval maximum.',
                'No probe content validation, unique-media streaming claim or physical desktop acceptance is implied.',
            ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=Path(__file__).with_name('mmc_read_cost_probe.py'))
    args = parser.parse_args()
    try:
        record = observe(args.file, args.probe)
    except Exception as error:
        print(json.dumps({'schema': 't736-mmc-timing-collection-v1', 'status': 'FAILED', 'error': str(error)}, indent=2))
        return 2
    print(json.dumps(record, indent=2))
    return 0 if record['status'] == 'OBSERVATION_COMPLETE' else 2


if __name__ == '__main__':
    raise SystemExit(main())
