#!/usr/bin/env python3
"""Build an isolated T736 Cedar 2.0/OpenMAX candidate; never install or run it.

Run on the Linux build host. Tina inputs are read-only. The output includes
proprietary libraries and is a private experiment artifact, not a source upload.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tina', type=Path, default=Path('/home/ubuntu/t736-aiot/tina'))
    ap.add_argument('--work', type=Path, required=True, help='New isolated output directory')
    args = ap.parse_args()
    tina, work = args.tina.resolve(), args.work.resolve()
    if work.exists() or work == tina or tina in work.parents:
        ap.error('--work must be a new directory outside Tina')
    sdk = tina / 'out/t736/demo_aiot/buildroot/buildroot/host'
    archive = tina / 'buildroot/buildroot-202205/dl/gst-omx/gst-omx-1.20.1.tar.xz'
    recipe = tina / 'buildroot/buildroot-202205/package/gstreamer1/gst-omx'
    patches = sorted(recipe.glob('*.patch'))
    cedar_deb = tina / 'debian/packages/arm64/libcedarc/libcedarc-dev_2.0.0_arm64.deb'
    expected_cedar = '229b69699184bbf7b0bade1540cc06cda50e20ccb5cd6da9f8c365d1bc1023d0'
    assert sha(cedar_deb) == expected_cedar, 'Cedar package changed; re-audit required'
    assert len(patches) == 19, 'Unexpected gst-omx patch series'
    src, build, logs, cedar = (work / x for x in ('src', 'build', 'logs', 'cedar'))
    src.mkdir(parents=True)
    logs.mkdir()
    env = dict(os.environ, PATH=str(sdk / 'bin') + os.pathsep + os.environ['PATH'])

    def run(argv, log, cwd=None):
        with (logs / log).open('ab') as out:
            subprocess.run([str(x) for x in argv], cwd=cwd, env=env,
                           stdout=out, stderr=subprocess.STDOUT, check=True)

    run(['tar', '-xf', archive, '--strip-components=1', '-C', src], 'patch.log')
    for patch in patches:
        run(['patch', '-p1', '--batch', '-i', patch], 'patch.log', src)
    for config in (src / 'config').rglob('gstomx.conf'):
        config.write_text(config.read_text().replace('/usr/local', '/usr').replace('/opt/vc', '/usr'))
    cross = sdk / 'etc/meson/cross-compilation.conf'
    run([sdk / 'bin/meson', 'setup', build, src, '--cross-file', cross,
         '--prefix=/usr', '--libdir=lib', '-Dtarget=buildroot', '-Dstruct_packing=8',
         '-Dexamples=disabled', '-Dtests=disabled', '-Dtools=disabled', '-Ddoc=disabled'], 'configure.log')
    run([sdk / 'bin/ninja', '-C', build, '-j4'], 'build.log')
    run(['dpkg-deb', '-x', cedar_deb, cedar], 'cedar-extract.log')
    cedar_src = tina / 'platform/allwinner/multimedia/libcedarc_v2.0'
    lib = cedar / 'usr/lib/aarch64-linux-gnu'
    demo_src = cedar_src / 'demo/vdecoderDemo'
    demo = work / 'vdecoderdemo'
    compiler = sdk / 'bin/aarch64-none-linux-gnu-gcc'
    run([compiler, '-O2', '-g', '-I' + str(cedar / 'usr/include'),
         '-I' + str(cedar_src / 'base/include'), '-I' + str(demo_src),
         demo_src / 'vdecoderDemo.c', demo_src / 'rawStreamParser.c', '-o', demo,
         '-L' + str(lib), '-Wl,-rpath-link,' + str(lib), '-lcdc_base', '-lMemAdapter',
         '-lvdecoder', '-lfbm', '-lsbm', '-lpthread', '-lm', '-lrt', '-ldl', '-lVE',
         '-lvideoengine'], 'demo-build.log')
    bundle = work / 't736-cedar-bridge-v2'
    for name in ('bin', 'lib/gstreamer-1.0', 'config'):
        (bundle / name).mkdir(parents=True)
    shutil.copy2(build / 'omx/libgstomx.so', bundle / 'lib/gstreamer-1.0/libgstomx.so')
    shutil.copy2(demo, bundle / 'bin/vdecoderdemo')
    decoder_names = ['libVE.so', 'libcdc_base.so', 'libMemAdapter.so', 'libfbm.so',
                     'libsbm.so', 'libvideoengine.so', 'libvdecoder.so', 'libvdecVcs.so',
                     'libscaledown.so', 'libOmxCore.so', 'libOmxVdec.so']
    for file in sorted(set([lib / name for name in decoder_names] + list(lib.glob('libaw*.so')))):
        shutil.copy2(file, bundle / 'lib' / file.name)
    config = '''[omxh264dec]
type-name=GstOMXH264Dec
core-name=libOmxCore.so
component-name=OMX.allwinner.video.decoder.avc
rank=0
in-port-index=0
out-port-index=1
hacks=event-port-settings-changed-ndata-parameter-swap;event-port-settings-changed-port-0-to-1;no-disable-outport
'''
    (bundle / 'config/gstomx.conf').write_text(config)
    # This file is a reference only: the unmodified Cedar base reads /etc/cedarc.conf.
    shutil.copy2(cedar / 'etc/cedarc.conf', bundle / 'config/cedarc.conf.reference')
    wrapper_path = Path(__file__).parent / 'validation/cedar-env'
    wrapper = wrapper_path.read_text()
    (bundle / 'bin/cedar-env').write_text(wrapper)
    (bundle / 'bin/cedar-env').chmod(0o755)
    run(['sh', '-n', bundle / 'bin/cedar-env'], 'wrapper-check.log')
    (bundle / 'README.txt').write_text(
        'Private T736 Cedar 2.0 candidate. Build-only; no hardware acceptance.\n'
        'Contains proprietary Tina decoder libraries; do not publish this archive.\n'
        'Extract to a private directory; no ldconfig/system plugin installation.\n'
        'bin/cedar-env gst-inspect-1.0 omxh264dec checks plugin registration only.\n'
        'H264 is rank 0 and must be explicitly selected. Use afbc=false initially.\n'
        'No Firefox/VAAPI integration is implemented.\n'
        'Decoder use requires an accepted /dev/cedar_dev, dma-heap and VE kernel path.\n'
        'libvideoengine derives the codec directory from libvdecoder in /proc/self/maps.\n'
        'Cedar reads /etc/cedarc.conf; config/cedarc.conf.reference is not auto-loaded.\n'
        'The built-in numeric defaults are used if that global file is absent.\n'
        'This bundle does not modify that file; record any existing board configuration.\n')
    files = []
    for file in sorted(bundle.rglob('*')):
        if not file.is_file():
            continue
        item = {'path': file.relative_to(bundle).as_posix(), 'bytes': file.stat().st_size, 'sha256': sha(file)}
        if file.read_bytes()[:4] == b'\x7fELF':
            dynamic = subprocess.check_output(['readelf', '-d', str(file)], text=True)
            versions = subprocess.check_output(['readelf', '--version-info', str(file)], text=True)
            item['needed'] = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
            item['glibc_versions'] = sorted(set(re.findall(r'GLIBC_\d+(?:\.\d+)+', versions)))
            item['loader_paths'] = re.findall(r'\((?:RPATH|RUNPATH)\).*\[(.*?)\]', dynamic)
        files.append(item)
    manifest = {
        'schema': 1, 'status': 'compiled; not installed or executed on target',
        'tina_input_read_only': True, 'gst_omx_version': '1.20.1',
        'cedar_package_sha256': expected_cedar,
        'source_archive_sha256': sha(archive),
        'patches': [{'path': p.relative_to(tina).as_posix(), 'sha256': sha(p)} for p in patches],
        'cross_file_sha256': sha(cross),
        'demo_sources': [{'path': p.relative_to(tina).as_posix(), 'sha256': sha(p)}
                         for p in sorted(demo_src.glob('*')) if p.suffix in ('.c', '.h')],
        'builder_sha256': sha(Path(__file__)),
        'wrapper_sha256': sha(wrapper_path),
        'compiler': subprocess.check_output([str(compiler), '--version'], text=True).splitlines()[0],
        'target_kernel_abi_verified': False, 'firefox_hardware_decode_verified': False,
        'gst_stock_debian_runtime_verified': False, 'files': files,
    }
    (bundle / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    package = work / (bundle.name + '.tar.gz')
    with tarfile.open(package, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    result = {'package': str(package), 'bytes': package.stat().st_size, 'sha256': sha(package),
              'manifest_sha256': sha(bundle / 'manifest.json'), 'files': len(files)}
    (work / 'build-record.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
