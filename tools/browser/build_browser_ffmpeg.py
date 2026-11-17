#!/usr/bin/env python3
"""Build isolated ARM64 FFmpeg 7.1.5 libraries with Cedar client codecs."""
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Extracted official FFmpeg n7.1.5 source')
    parser.add_argument('--work', type=Path, required=True, help='New isolated build directory')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--resume', action='store_true', help='Rebuild the same isolated work directory after wrapper changes')
    parser.add_argument('--bundle-name', default='t736-browser-ffmpeg-v1', help='Separate artifact name for an incremental candidate')
    args = parser.parse_args()
    if not re.fullmatch(r't736-browser-ffmpeg-v[0-9]+', args.bundle_name):
        parser.error('bundle-name must have the form t736-browser-ffmpeg-vN')
    source, work = args.source.resolve(), args.work.resolve()
    if source == work or source in work.parents or work in source.parents:
        parser.error('source and work must be separate directory trees')
    if (source/'RELEASE').read_text(encoding='utf-8').strip() != '7.1.5':
        parser.error('only the reviewed FFmpeg 7.1.5 source is supported')
    if work.exists() and not args.resume:
        parser.error('work must be new unless --resume is specified')
    patched, build, install = work/'src', work/'build', work/'install'
    if args.resume:
        if not (build/'config.h').is_file() or not (patched/'libavcodec/ffmpeg_cedar.c').is_file():
            parser.error('--resume requires a previous build from this builder')
        if (patched/'RELEASE').read_text(encoding='utf-8').strip() != '7.1.5':
            parser.error('unexpected version in existing work directory')
    else:
        work.mkdir(parents=True)
        shutil.copytree(source, patched)
        build.mkdir()
    here = Path(__file__).resolve().parent
    for name in ('ffmpeg_cedar.c','cedar_client.h'):
        shutil.copy2(here/name, patched/'libavcodec'/name)
    makefile = patched/'libavcodec/Makefile'
    if not args.resume:
        makefile.write_text(makefile.read_text(encoding='utf-8') +
            '\nOBJS-$(CONFIG_H264_CEDAR_DECODER) += ffmpeg_cedar.o\nOBJS-$(CONFIG_HEVC_CEDAR_DECODER) += ffmpeg_cedar.o\n', encoding='utf-8')
    allcodecs = patched/'libavcodec/allcodecs.c'
    text = allcodecs.read_text(encoding='utf-8')
    for codec in ('h264','hevc'):
        anchor = f'extern const FFCodec ff_{codec}_decoder;'
        if text.count(anchor) != 1:
            raise ValueError('Unexpected allcodecs layout')
        cedar = f'extern const FFCodec ff_{codec}_cedar_decoder;'
        if cedar not in text:
            text = text.replace(anchor, cedar+'\n'+anchor)
    allcodecs.write_text(text, encoding='utf-8')
    command = [str(patched/'configure'), '--prefix=/opt/t736-browser-ffmpeg',
               '--arch=aarch64', '--target-os=linux', '--enable-cross-compile',
               '--cross-prefix=aarch64-linux-gnu-', '--enable-shared', '--disable-static',
               '--enable-pic', '--disable-doc', '--disable-programs', '--disable-autodetect',
               '--disable-network', '--disable-encoders', '--disable-muxers', '--disable-devices',
               '--disable-filters', '--disable-avdevice', '--disable-avfilter', '--disable-postproc',
               '--extra-ldflags=-ldl', '--enable-decoder=h264_cedar,hevc_cedar,h264,hevc',
               '--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb']
    if not args.resume:
        with (work/'configure.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    codecs = build/'libavcodec/codec_list.c'
    text = codecs.read_text(encoding='utf-8')
    for codec in ('h264','hevc'):
        line = f'    &ff_{codec}_cedar_decoder,\n'
        if text.count(line) != 1:
            raise ValueError('New decoder was not enabled')
        text = text.replace(line, '')
    text = text.replace('{\n', '{\n    &ff_h264_cedar_decoder,\n    &ff_hevc_cedar_decoder,\n', 1)
    codecs.write_text(text, encoding='utf-8')
    with (work/'build.log').open('w', encoding='utf-8') as log:
        subprocess.run(['make','-j'+str(args.jobs)], cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (work/'install.log').open('w', encoding='utf-8') as log:
        subprocess.run(['make','install','DESTDIR='+str(install)], cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    bundle = work/args.bundle_name
    (bundle/'lib').mkdir(parents=True, exist_ok=True)
    for path in (install/'opt/t736-browser-ffmpeg/lib').glob('*.so*'):
        destination = bundle/'lib'/path.name
        if destination.is_symlink():
            destination.unlink()
        shutil.copy2(path, destination, follow_symlinks=False)
    libraries = {}
    for path in sorted((bundle/'lib').glob('*.so.*.*.*')):
        dynamic = subprocess.check_output(['aarch64-linux-gnu-readelf','-d',str(path)], text=True)
        libraries[path.name] = {'bytes':path.stat().st_size,'sha256':sha(path),
                               'needed':re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic),
                               'soname':re.findall(r'\(SONAME\).*\[(.*?)\]',dynamic)}
    record = {'schema':1,'status':'COMPILED_TESTS_AND_BOARD_PENDING',
              'upstream':'FFmpeg official n7.1.5 tag',
              'source_archive_sha256':'e3963a50831c985933e1a625ed566ec4c7adb5c012c34fa9f84438e1d61bdacc',
              'source_archive_url':'https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.5',
              'configure':command, 'source_sha256':sha(patched/'libavcodec/ffmpeg_cedar.c'),
              'client_header_sha256':sha(patched/'libavcodec/cedar_client.h'),
              'codec_list_sha256':sha(codecs), 'libraries':libraries,
              'selection':'Cedar wrappers lead av_codec_iterate and avcodec_find_decoder; native names h264/hevc remain available for fallback.',
              'software_codecs':'All available in-tree decoders/parsers retained; external codec libraries not auto-detected or linked. Native h264/hevc and audio fallback retained.',
              'sandbox':'No sandbox disabling. Hardware API resolved only from an already-preloaded, already-connected client.',
              'limits':['Only even <=1920x1080 8-bit hardware frames accepted.',
                        'Bounded replay fallback only before a hardware frame is delivered; post-delivery hardware errors remain explicit.',
                        'No assertion of hardware use based on Firefox software-frame backend labels.',
                        'No system library installation or target execution by this builder.']}
    (bundle/'manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    archive=work/(args.bundle_name+'.tar.gz')
    with tarfile.open(archive,'w:gz') as tar:
        tar.add(bundle,arcname=bundle.name)
    result={'archive':str(archive),'bytes':archive.stat().st_size,'sha256':sha(archive),
            'manifest_sha256':sha(bundle/'manifest.json'),'libraries':libraries}
    (work/'build-result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
