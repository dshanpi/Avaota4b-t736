#!/usr/bin/env python3
"""Build and test the isolated ordinary-RAM Cedar v2 transport; never deploy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    p.add_argument('--work', type=Path, required=True)
    a = p.parse_args()
    root, work = a.root.resolve(), a.work.resolve()
    if root not in work.parents or (work/'build-record.json').exists():
        p.error('use an unfinished independent work directory inside the workspace')
    work.mkdir(parents=True, exist_ok=True)
    src = root/'tools/browser/shm_v2'
    assert (src/'cedar_client.h').read_bytes() == (root/'tools/browser/cedar_client.h').read_bytes()
    assert (src/'cedar_backend.h').read_bytes() == (root/'tools/browser/cedar_backend.h').read_bytes()
    v1_files = [root/'tools/browser'/name for name in ('cedar_client.c', 'cedar_broker.c', 'cedar_protocol.h')]
    v1_hashes = {str(f.relative_to(root)): sha(f) for f in v1_files}
    inputs = sorted(f for f in src.iterdir() if f.is_file()) + [
        root/'tests/test_cedar_shm_transport.c', root/'tests/test_cedar_shm_seals.c',
        root/'tests/test_cedar_shm_client_fork.c', Path(__file__).resolve()]
    source_hashes = {str(f.relative_to(root)): sha(f) for f in inputs}
    sdk = Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    cc = sdk/'bin/aarch64-none-linux-gnu-gcc'
    sysroot = sdk/'aarch64-buildroot-linux-gnu/sysroot'
    backend = root/'build/cedar-browser-backend-v5-20261117/libcedar_backend.so'
    expected_backend = '52bf715c24ac9c072c1550f04fe47ab4df195ffdf1b51c4ac7f84414556f8f08'
    assert sha(backend) == expected_backend
    common = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread']
    commands = []

    def run(name, argv, env=None):
        argv = [str(x) for x in argv]
        with (work/(name+'.log')).open('w') as stream:
            subprocess.run(argv, check=True, stdout=stream, stderr=subprocess.STDOUT,
                           env=env, timeout=180)
        commands.append({'name': name, 'argv': argv})

    client = work/'libt736_cedar_client.so'
    broker = work/'cedar-broker'
    run('arm64-client', [cc, *common, '-O2', '-fPIC', '-shared', src/'cedar_client.c',
        '-Wl,-z,relro,-z,now,-soname,libt736_cedar_client.so', '-o', client])
    run('arm64-broker', [cc, *common, '-O2', '-fPIE', '-pie', src/'cedar_broker.c',
        '-L'+str(backend.parent), '-lcedar_backend', '-Wl,-rpath,$ORIGIN/../lib',
        '-Wl,-rpath-link,'+str(sysroot/'usr/lib'), '-Wl,-z,relro,-z,now', '-o', broker])
    test_env = dict(os.environ)
    for key in ('LD_PRELOAD', 'T736_CEDAR_ENABLE', 'T736_CEDAR_TEST_CLIENT'):
        test_env.pop(key, None)
    sanitizers = dict(test_env, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                      UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for name in ('transport', 'seals', 'client_fork'):
        test = root/f'tests/test_cedar_shm_{name}.c'
        host = work/(name+'-sanitized')
        arm = work/(name+'-arm64')
        # The independent review also exercised the unmodified five-second
        # deadline; a short compile-time deadline keeps this regression bounded.
        extra = ['-DT736_SETUP_TIMEOUT_MS=200'] if name == 'seals' else []
        run('host-'+name+'-build', ['gcc', *common, *extra, '-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', test, '-o', host])
        run('host-'+name+'-test', [host], sanitizers)
        run('arm64-'+name+'-build', [cc, *common, *extra, '-O2', test, '-o', arm])
        if name == 'transport':
            run('arm64-'+name+'-test', ['/usr/bin/qemu-aarch64-static', '-L', sysroot, arm], test_env)
        else:
            # QEMU 8.2.2 linux-user returns success for MADV_DONTFORK while
            # retaining the mapping in its fork child. Do not weaken the tests
            # or production code to accommodate that emulation limitation.
            # These unmodified ARM64 binaries require a native ARM64 kernel.
            native = work/(name+'-native')
            run('host-'+name+'-native-build', ['gcc', *common, '-O2', test, '-o', native])
            run('host-'+name+'-native-test', [native], test_env)
    symbols = subprocess.check_output(['aarch64-linux-gnu-nm', '-D', '--defined-only', client], text=True)
    exports = sorted(line.split()[-1] for line in symbols.splitlines() if ' t736_client_' in line)
    expected = sorted('t736_client_'+x for x in ('available', 'open', 'push', 'pull', 'drain', 'flush', 'close'))
    assert exports == expected
    (work/'client-exports.txt').write_text(symbols)
    for name, binary in [('client', client), ('broker', broker)]:
        run(name+'-elf', ['aarch64-linux-gnu-readelf', '-h', '-d', binary])
    for rel, digest in source_hashes.items(): assert sha(root/rel) == digest
    for rel, digest in v1_hashes.items(): assert sha(root/rel) == digest
    bundle = work/'t736-cedar-transport-v2'
    (bundle/'bin').mkdir(parents=True)
    (bundle/'lib').mkdir()
    source_dir = bundle/'tools/browser/shm_v2'
    source_dir.mkdir(parents=True)
    (bundle/'tests').mkdir()
    shutil.copy2(client, bundle/'lib'/client.name)
    shutil.copy2(broker, bundle/'bin'/broker.name)
    for path in src.iterdir():
        if path.is_file(): shutil.copy2(path, source_dir/path.name)
    shutil.copy2(Path(__file__), bundle/'tools/build_cedar_shm_transport.py')
    for name in ('test_cedar_shm_transport.c', 'test_cedar_shm_seals.c', 'test_cedar_shm_client_fork.c'):
        shutil.copy2(root/'tests'/name, bundle/'tests'/name)
    shutil.copy2(src/'README.md', bundle/'README.md')
    record = {'schema': 1, 'status': 'COMPILED_NATIVE_ALL_AND_ARM64_TRANSPORT_QEMU_PASS_BOARD_PENDING',
        'board_access': False, 'deployment': False, 'protocol': 2,
        'client_ABI_unchanged': True, 'client_exports': exports,
        'required_backend_sha256': expected_backend, 'client_sha256': sha(client), 'broker_sha256': sha(broker),
        'commands': commands, 'sources': source_hashes, 'canonical_v1_preserved': v1_hashes,
        'test_scope': {
            'native_x86_64': 'Actual client/broker transport, helper seals and client fork/exec: ASan/UBSan/LSan PASS. Helper default5000ms deadline and fork/exec also native O2 PASS.',
            'arm64_qemu': 'Actual client/broker transport PASS. Helper seals and fork/exec cross-compiled but not accepted under QEMU8.2.2: MADV_DONTFORK is reported successful yet mapping survives fork, independently reproduced without Cedar code.',
            'arm64_native': 'Separate board-run evidence required; not performed by this builder.',
            'qemu_limitation_record': 'evidence/cedar-shm-qemu-dontfork-limitation-20261117.json'},
        'test_binaries': [{'path': str(f.relative_to(work)), 'bytes': f.stat().st_size, 'sha256': sha(f)}
                          for f in (work/'transport-arm64', work/'seals-arm64', work/'client_fork-arm64')],
        'transport': 'Per-peer ordinary memfd, 3110400 bytes, broker existing RW map, receiver RO DONTFORK map and closed descriptor, metadata-only PULL, memcpy into existing caller destination under client mutex',
        'seals': ['SHRINK', 'GROW', 'FUTURE_WRITE', 'SEAL', 'EXEC from MFD_NOEXEC_SEAL'],
        'bounds': {'peers': 8, 'bytes_per_peer': 3110400, 'setup_absolute_deadline_ms': 5000},
        'limits': ['No device, DMA fd or hardware address transferred', 'No sandbox weakening or post-sandbox reconnect',
                   'Matched v2 broker/client required; v1 refused without silent downgrade',
                   'No AVFrame ownership change; two ordinary RAM copies after backend output remain',
                   'No Firefox or board performance acceptance; not a claim to fix VideoSink late-frame throttling',
                   'Full helper/fork ARM64 QEMU runtime is not accepted: emulator does not implement DONTFORK semantics; native ARM64 results must be recorded separately'],
        'files': [{'path': str(f.relative_to(bundle)), 'bytes': f.stat().st_size, 'sha256': sha(f)}
                  for f in sorted(bundle.rglob('*')) if f.is_file()]}
    (bundle/'manifest.json').write_text(json.dumps(record, indent=2)+'\n')
    archive = work/'t736-cedar-transport-v2.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar: tar.add(bundle, arcname=bundle.name)
    record.update(package=str(archive), package_sha256=sha(archive), bytes=archive.stat().st_size)
    (work/'build-record.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({key: record[key] for key in ('package', 'package_sha256', 'bytes', 'client_sha256', 'broker_sha256', 'status')}))


if __name__ == '__main__':
    main()
