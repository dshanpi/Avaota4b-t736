#!/usr/bin/env python3
"""Build the private Cedar browser backend and run device-free copy/flush tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    root, work = args.root.resolve(), args.work.resolve()
    if root not in work.parents:
        parser.error('work must be inside mainline workspace')
    if (work/'build-record.json').exists():
        parser.error('completed work directory is frozen; use a new candidate directory')
    work.mkdir(parents=True, exist_ok=True)
    sdk = Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    sysroot = sdk/'aarch64-buildroot-linux-gnu/sysroot'
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(sysroot),
               PKG_CONFIG_LIBDIR=f'{sysroot}/usr/lib/pkgconfig:{sysroot}/usr/share/pkgconfig')
    modules = ['gstreamer-app-1.0', 'gstreamer-video-1.0']
    flags = shlex.split(subprocess.check_output([str(sdk/'bin/pkg-config'), '--cflags', '--libs', *modules], env=env, text=True))
    cflags = shlex.split(subprocess.check_output([str(sdk/'bin/pkg-config'), '--cflags', *modules], env=env, text=True))
    cc = str(sdk/'bin/aarch64-none-linux-gnu-gcc')
    source = root/'tools/browser/cedar_backend.c'
    test = root/'tests/test_cedar_backend.c'
    common = ['-std=c11', '-Wall', '-Wextra', '-Werror']
    commands = []

    def run(name, cmd, runenv=None):
        commands.append({'name': name, 'argv': cmd})
        with (work/(name+'.log')).open('w') as log:
            subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT,
                           env=runenv or env, cwd=root, timeout=120)

    lib = work/'libcedar_backend.so'
    run('arm64-build', [cc, *common, '-O2', '-fPIC', '-shared', str(source), *flags,
                        '-Wl,-z,relro,-z,now', '-o', str(lib)])
    armtest = work/'test-copy-arm64'
    run('arm64-test-build', [cc, *common, '-O2', str(test), *flags, '-o', str(armtest)])
    testenv = dict(env, GST_PLUGIN_SYSTEM_PATH_1_0='', GST_PLUGIN_PATH_1_0='',
                   GST_REGISTRY_1_0=str(work/'test-registry.bin'))
    run('arm64-qemu-test', ['/usr/bin/qemu-aarch64-static', '-L', str(sysroot), str(armtest)], testenv)
    # Public Gst 1.x headers are ABI-stable. The SDK's GLib config uses the same
    # LP64 sizes as the x86_64 host; idirafter preserves native libc headers.
    hostflags = []
    for flag in cflags:
        hostflags.extend(['-idirafter', flag[2:]] if flag.startswith('-I') else [flag])
    hosttest = work/'test-copy-sanitized'
    hostlibs = ['-l:libgstapp-1.0.so.0', '-l:libgstvideo-1.0.so.0',
                '-l:libgstreamer-1.0.so.0', '-l:libgobject-2.0.so.0', '-l:libglib-2.0.so.0']
    run('host-test-build', ['gcc', *common, '-O1', '-g', '-fsanitize=address,undefined',
                            str(test), *hostflags, *hostlibs, '-o', str(hosttest)])
    run('host-sanitizer-test', [str(hosttest)], dict(testenv, ASAN_OPTIONS='detect_leaks=0'))
    hostbench = work/'test-copy-optimized'
    run('host-optimized-build', ['gcc', *common, '-O2', str(test),
                                  *hostflags, *hostlibs, '-o', str(hostbench)])
    for mode in ('malloc', 'pool'):
        run('host-ram-bench-'+mode, [str(hostbench), '--ram-bench-'+mode], testenv)
    bundle = work/'t736-cedar-browser-backend-v5'
    bundle.mkdir(exist_ok=True)
    for file in (source, source.with_suffix('.h'), lib):
        shutil.copy2(file, bundle/file.name)
    (bundle/'README.txt').write_text(
        'Experimental native Cedar browser broker backend; not browser acceptance.\n'
        'Requires matching private WC-v4 gst-omx crop metadata and t736-set-idle action.\n'
        'Only progressive 8-bit H264/HEVC Annex-B AUs up to 1920x1080.\n'
        'One externally serialized, single-active decoder process; retain one same-format pipeline across leases.\n'
        'cedar_flush uses a flushing seek, preserving the pipeline and normal same-format OMX/FBM pools.\n'
        'At lease close call cedar_idle: flush -> Gst PAUSED -> explicit OMX Pause. Next push resumes automatically.\n'
        'This idle operation discards data and is not an ordinary lossless player pause.\n'
        'AArch64 NV12/NV21 chroma splitting uses NEON; I420/YV12 remain row memcpy.\n'
        'CEDAR_COPY logs first frame and every300 frames: format/strides, map and allocation+copy times.\n'
        'Preserves and logs the first backend/GStreamer error, including component state diagnostics; no poison recovery.\n'
        'Ordinary I420 RAM output uses a private reference-counted pool with at most2 idle buffers.\n'
        'Each delivered frame owns its block until cedar_frame_free; pixels survive later pulls, flush and decoder close.\n'
        'cedar_frame ABI is unchanged; release only through this backend library, never free(data).\n'
        'RAM allocator benchmark uses ordinary host RAM, not hardware WC memory or a playback benchmark.\n'
        'Source review does not establish AXI DMA drain or hardware flush acceptance.\n'
        'The bring-up kernel pins all imported DMA memory until reboot; do not create/destroy per lease.\n'
        'No VDPAU, VAAPI, foreign GPU buffer import, hardware fd transfer, or browser sandbox changes.\n'
        'An OMX/GStreamer error poisons the instance; keep it retained and reject further work.\n')
    record = {'schema': 1, 'status': 'COMPILED_DEVICE_FREE_TESTS_PASS_HARDWARE_PENDING',
              'board_access': False, 'commands': commands,
              'tests': ['ARM64 QEMU actual GstVideoFrame copy path and software EOS-flush-reuse',
                        'native actual C ASan/UBSan, leak detection disabled for GStreamer process globals',
                        'Owned output survives actual pull/flush/close; 1000 reuse cycles, idle cache bound, concurrent frame return/close',
                        'Optimized native ordinary-RAM allocation/copy benchmark and minor/major fault counts'],
              'hardware_not_tested': ['OMX pool reuse', 'actual crop query', 'real Cedar decoding', 'Firefox'],
              'sources': [{'path': str(p.relative_to(root)), 'sha256': sha(p)}
                          for p in (source, source.with_suffix('.h'), test)],
              'library_sha256': sha(lib),
              'files': [{'path': p.name, 'bytes': p.stat().st_size, 'sha256': sha(p)}
                        for p in sorted(bundle.iterdir()) if p.is_file() and p.name != 'manifest.json']}
    (bundle/'manifest.json').write_text(json.dumps(record, indent=2)+'\n')
    archive = work/'t736-cedar-browser-backend-v5.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    result = dict(record, package=str(archive), package_sha256=sha(archive), bytes=archive.stat().st_size)
    (work/'build-record.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'package': str(archive), 'sha256': sha(archive),
                      'library_sha256': sha(lib), 'status': record['status']}))


if __name__ == '__main__':
    main()
