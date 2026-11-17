#!/usr/bin/env python3
"""Private WC-v2 package: fix borrowed DMA fd ownership in gst-omx only."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    ap.add_argument('--work', type=Path, required=True)
    args = ap.parse_args()
    root, work = args.root.resolve(), args.work.resolve()
    baseline = root / 'build/cedar-wc-v1-r2-20261117/t736-cedar-wc-v1'
    donor = root / 'build/cedar-bridge-package-v2'
    sdk = Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    if work.exists() or root not in work.parents:
        ap.error('work must be a new directory inside the mainline workspace')
    assert sha(baseline / 'manifest.json') == '351878fa95940041f61495d30cf7914b298cb733f2a08b7d691a16e74f73fa81'
    work.mkdir(parents=True)
    source, build = work / 'src', work / 'build'
    shutil.copytree(donor / 'src', source)
    p = source / 'omx/gstomxallocator.c'
    old = p.read_text()
    changed = old.replace('#include <gst/allocators/gstdmabuf.h>',
                          '#include <gst/allocators/gstdmabuf.h>\n#include <errno.h>\n#include <fcntl.h>\n#include <unistd.h>', 1)
    before = '''      gint fd = sOmxOutPrivateBuffer->nShareBufFd;
      mem->foreign_mem =
          gst_dmabuf_allocator_alloc (allocator->foreign_allocator, fd,
          omx_buf->omx_buf->nAllocLen);
'''
    after = '''      gint fd = sOmxOutPrivateBuffer->nShareBufFd;
      gint owned_fd;

      /* OpenMAX/MemAdapter owns fd. GstFdMemory takes ownership of its fd
       * and closes it on destruction, so transfer a distinct descriptor. */
      do {
        owned_fd = fcntl (fd, F_DUPFD_CLOEXEC, 0);
      } while (owned_fd < 0 && errno == EINTR);
      if (owned_fd < 0) {
        GST_ERROR_OBJECT (allocator, "Cannot duplicate borrowed OMX DMA fd: %s",
            g_strerror (errno));
        gst_memory_unref ((GstMemory *) mem);
        return NULL;
      }
      mem->foreign_mem =
          gst_dmabuf_allocator_alloc (allocator->foreign_allocator, owned_fd,
          omx_buf->omx_buf->nAllocLen);
      if (!mem->foreign_mem) {
        close (owned_fd);
        gst_memory_unref ((GstMemory *) mem);
        return NULL;
      }
'''
    assert changed.count(before) == 1
    changed = changed.replace(before, after, 1)
    p.write_text(changed)
    patch = work / 'gstomx-dmabuf-fd-ownership.patch'
    patch.write_text(''.join(difflib.unified_diff(old.splitlines(True), changed.splitlines(True),
                                               'a/omx/gstomxallocator.c', 'b/omx/gstomxallocator.c')))
    env = dict(os.environ, PATH=str(sdk / 'bin') + os.pathsep + os.environ['PATH'])
    commands = [
        [str(sdk/'bin/meson'), 'setup', str(build), str(source), '--cross-file',
         str(sdk/'etc/meson/cross-compilation.conf'), '--prefix=/usr', '--libdir=lib',
         '-Dtarget=buildroot', '-Dstruct_packing=8', '-Dexamples=disabled',
         '-Dtests=disabled', '-Dtools=disabled', '-Ddoc=disabled'],
        [str(sdk/'bin/ninja'), '-C', str(build), '-j4'],
    ]
    for name, cmd in zip(('configure', 'build'), commands):
        with (work/(name+'.log')).open('w') as out:
            subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, env=env, check=True)
    bundle = work / 't736-cedar-wc-v2'
    shutil.copytree(baseline, bundle)
    (bundle/'manifest.json').rename(bundle/'BASELINE-WC-V1-MANIFEST.json')
    plugin_rel = 'lib/gstreamer-1.0/libgstomx.so'
    shutil.copy2(build/'omx/libgstomx.so', bundle/plugin_rel)
    (bundle/'source').mkdir(exist_ok=True)
    shutil.copy2(patch, bundle/'source'/patch.name)
    unchanged = []
    for src in sorted((baseline/'lib').rglob('*')):
        if not src.is_file() or src.relative_to(baseline).as_posix() == plugin_rel:
            continue
        dst = bundle/src.relative_to(baseline)
        assert sha(src) == sha(dst)
        unchanged.append({'path': src.relative_to(baseline).as_posix(), 'sha256': sha(src)})
    metadata = {
        'schema': 1, 'status': 'COMPILED_HARDWARE_PENDING', 'board_access': False,
        'change': 'duplicate borrowed OpenMAX DMA fd with F_DUPFD_CLOEXEC before transfer to GstMemory',
        'baseline_manifest_sha256': sha(baseline/'manifest.json'),
        'source_before_sha256': sha(donor/'src/omx/gstomxallocator.c'),
        'source_after_sha256': sha(p), 'patch_sha256': sha(patch),
        'plugin_before_sha256': sha(baseline/plugin_rel),
        'plugin_after_sha256': sha(bundle/plugin_rel),
        'unchanged_libraries': unchanged, 'commands': commands,
        'failure_paths': ['dup failure frees unpublished wrapper without closing borrowed fd',
                          'GstMemory allocation failure closes only owned duplicate and frees wrapper'],
        'kernel_and_memadapter_unchanged': True,
    }
    (bundle/'FD-OWNERSHIP-MANIFEST.json').write_text(json.dumps(metadata, indent=2)+'\n')
    (bundle/'README.txt').write_text(
        'Private T736 Cedar WC-v2: WC-v1/r2 with gst-omx fd-ownership fix only.\n'
        'GstMemory owns a CLOEXEC duplicate; original DMA fd remains owned by MemAdapter.\n'
        'Vendor libraries, MemAdapter, decoder demo and kernel are unchanged.\n'
        'Requires matching experimental /dev/dma_heap/t736-ve; imported backing remains pinned until reboot.\n'
        'Contains proprietary libraries: do not publish the archive. Source patch may be reviewed separately.\n'
        'Candidate build does not itself establish target teardown or video acceptance.\n')
    manifest = {'schema': 1, 'kind': 't736-cedar-wc-v2', 'status': metadata['status'],
                'files': [{'path': q.relative_to(bundle).as_posix(), 'bytes': q.stat().st_size,
                           'sha256': sha(q)} for q in sorted(bundle.rglob('*')) if q.is_file()]}
    (bundle/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    archive = work / 't736-cedar-wc-v2.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    result = {'schema': 1, 'package': str(archive), 'bytes': archive.stat().st_size,
              'sha256': sha(archive), 'manifest_sha256': sha(bundle/'manifest.json'),
              'plugin_sha256': sha(bundle/plugin_rel), 'source_patch_sha256': sha(patch)}
    (work/'build-record.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
