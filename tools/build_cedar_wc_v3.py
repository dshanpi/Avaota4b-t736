#!/usr/bin/env python3
"""Build isolated WC-v3: retain v2 fd ownership and expose validated OMX crop."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile


BASE_SOURCE_SHA = 'd1b6426a75f60f8cc4df0f35e99a0ff31973dc23c9ec872b72891fa6c091f7f5'
BASE_ALLOCATOR_SHA = '19fceea8e5598e67af7ad9b70678a8db33aa0c995cf4689e77869efdaa459e72'
BASE_MANIFEST_SHA = 'd70a37314ab0d4d47a6e5e40e1898ef7e57b334e9be0379d098bd1161e14a591'

CROP_VALIDATOR = r'''static gboolean
gst_omx_video_dec_crop_valid (const OMX_CONFIG_RECTTYPE * crop,
    guint64 width, guint64 height)
{
  /* Subtraction after bounds checks avoids overflow in x + width/y + height.
   * GstVideoCropMeta fields are guint, even if a vendor OMX uses wider types. */
  if (!width || !height || width > G_MAXUINT || height > G_MAXUINT ||
      crop->nLeft < 0 || crop->nTop < 0 ||
      !crop->nWidth || !crop->nHeight ||
      (guint64) crop->nLeft >= width || (guint64) crop->nTop >= height)
    return FALSE;

  return (guint64) crop->nWidth <= width - (guint64) crop->nLeft &&
      (guint64) crop->nHeight <= height - (guint64) crop->nTop;
}
'''

CROP_HELPERS = CROP_VALIDATOR + r'''
static gboolean
gst_omx_video_dec_snapshot_crop (GstOMXVideoDec * self,
    OMX_CONFIG_RECTTYPE * crop)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_ERRORTYPE err;

  /* Query each nonempty output before any pool buffer can be copied/released.
   * The vendor updates this rectangle before announcing port changes. */
  GST_OMX_INIT_STRUCT (crop);
  crop->nPortIndex = self->dec_out_port->index;
  err = gst_omx_component_get_config (self->dec,
      OMX_IndexConfigCommonOutputCrop, crop);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Cannot query output crop: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  err = gst_omx_port_get_port_definition (self->dec_out_port, &port_def);
  if (err != OMX_ErrorNone ||
      !gst_omx_video_dec_crop_valid (crop,
          port_def.format.video.nFrameWidth,
          port_def.format.video.nFrameHeight)) {
    GST_ERROR_OBJECT (self, "Output crop is outside the OMX frame storage");
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_omx_video_dec_apply_crop (GstOMXVideoDec * self, GstBuffer * buf,
    const OMX_CONFIG_RECTTYPE * crop)
{
  GstVideoCropMeta *meta;

  if (!gst_buffer_is_writable (buf)) {
    GST_ERROR_OBJECT (self, "Output buffer is not writable for crop metadata");
    return FALSE;
  }
  meta = gst_buffer_get_video_crop_meta (buf);
  if (!meta)
    meta = gst_buffer_add_video_crop_meta (buf);
  if (!meta) {
    GST_ERROR_OBJECT (self, "Cannot attach output crop metadata");
    return FALSE;
  }
  meta->x = crop->nLeft;
  meta->y = crop->nTop;
  meta->width = crop->nWidth;
  meta->height = crop->nHeight;
  return TRUE;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Expected one source anchor, found {text.count(old)}: {old[:100]}')
    return text.replace(old, new, 1)


def patch_decoder(old):
    start = old.index('static void add_meta_to_output(')
    end = old.index('// static GstBuffer * wrapper_omx_buffer', start)
    text = old[:start] + CROP_HELPERS + '\n' + old[end:]
    text = replace_once(text,
        '  GstOMXAcquireBufferReturn acq_return;\n  OMX_ERRORTYPE err;\n',
        '  GstOMXAcquireBufferReturn acq_return;\n  OMX_ERRORTYPE err;\n'
        '  OMX_CONFIG_RECTTYPE output_crop;\n')
    text = replace_once(text,
        '  frame = gst_omx_video_find_nearest_frame (GST_ELEMENT_CAST (self), buf,\n',
        '''  /* Keep the crop snapshot independent of the OMX/pool buffer lifetime. */
  if ((buf->omx_buf->nFilledLen > 0 || buf->eglimage) &&
      !gst_omx_video_dec_snapshot_crop (self, &output_crop)) {
    gst_omx_port_release_buffer (port, buf);
    buf = NULL;
    goto crop_error;
  }

  frame = gst_omx_video_find_nearest_frame (GST_ELEMENT_CAST (self), buf,
''')
    text = replace_once(text,
        '    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);\n',
        '''    /* Covers unmatched frames with either pool or copied output. */
    if (!gst_omx_video_dec_apply_crop (self, outbuf, &output_crop)) {
      gst_buffer_unref (outbuf);
      if (buf)
        gst_omx_port_release_buffer (port, buf);
      buf = NULL;
      goto crop_error;
    }
    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
''')
    text = replace_once(text,
        '''      OmxPrivateBuffer *sOmxOutPrivateBuffer = (OmxPrivateBuffer*)(buf->omx_buf->pBuffer);

      add_meta_to_output(outbuf, sOmxOutPrivateBuffer);
''',
        '''      if (!gst_omx_video_dec_apply_crop (self, outbuf, &output_crop)) {
        /* The output GstBuffer owns the pool reference, including after copy. */
        gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
        frame = NULL;
        buf = NULL;
        goto crop_error;
      }
''')
    text = replace_once(text,
        '''#ifdef USE_OMX_TARGET_ZYNQ_USCALE_PLUS
        set_outbuffer_interlace_flags (buf, frame->output_buffer);
#endif

        flow_ret =
''',
        '''#ifdef USE_OMX_TARGET_ZYNQ_USCALE_PLUS
        set_outbuffer_interlace_flags (buf, frame->output_buffer);
#endif

        if (!gst_omx_video_dec_apply_crop (self, frame->output_buffer,
                &output_crop)) {
          gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
          frame = NULL;
          gst_omx_port_release_buffer (port, buf);
          buf = NULL;
          goto crop_error;
        }

        flow_ret =
''')
    text = replace_once(text, '\ninvalid_buffer:\n', '''
crop_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Missing, invalid, or unpublishable OpenMAX output crop"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_omx_video_dec_pause_loop (self, GST_FLOW_NOT_NEGOTIATED);
    return;
  }

invalid_buffer:
''')
    assert text.count('gst_omx_video_dec_apply_crop (self,') == 3
    assert text.count('gst_omx_video_dec_snapshot_crop (self,') == 1
    assert 'add_meta_to_output(' not in text
    return text


def test_crop_validator(source, work):
    """Compile the exact helper with real OMX types and test hostile rectangles."""
    test = r'''#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "OMX_Core.h"
#include "OMX_IVCommon.h"
typedef int gboolean;
typedef uint64_t guint64;
#define G_MAXUINT UINT_MAX
#define TRUE 1
#define FALSE 0
'''
    # Extract the compiled implementation, rather than test a Python rewrite.
    actual = (source/'omx/gstomxvideodec.c').read_text()
    first = actual.index('static gboolean\ngst_omx_video_dec_crop_valid')
    last = actual.index('\nstatic gboolean\ngst_omx_video_dec_snapshot_crop', first)
    test += actual[first:last]
    test += r'''
int main(void) {
  OMX_CONFIG_RECTTYPE r = {0};
  r.nWidth=1920; r.nHeight=1080;
  assert(gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nLeft=4; r.nTop=2; r.nWidth=1916; r.nHeight=1086;
  assert(gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nWidth++; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nWidth--; r.nHeight++; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nHeight--; r.nLeft=-1; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nLeft=0; r.nTop=-1; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nTop=0; r.nWidth=0; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nWidth=1; r.nHeight=0; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nHeight=1; r.nLeft=1920; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nLeft=0; r.nTop=1088; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nTop=0; assert(!gst_omx_video_dec_crop_valid(&r,0,1088));
  assert(!gst_omx_video_dec_crop_valid(&r,1920,0));
  r.nWidth=(OMX_U32)-1; assert(!gst_omx_video_dec_crop_valid(&r,1920,1088));
  r.nWidth=1; assert(!gst_omx_video_dec_crop_valid(&r,(uint64_t)UINT_MAX+1,1088));
  puts("PASS: extracted crop validator, 14 boundary/offset cases");
  return 0;
}
'''
    path = work/'crop-validator-test.c'
    path.write_text(test)
    cmd = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-I'+str(source/'omx/openmax'), str(path), '-o', str(work/'crop-validator-test')]
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(work/'crop-validator-test')], text=True,
                            capture_output=True, check=True)
    (work/'crop-validator-test.log').write_text(result.stdout + result.stderr)
    return {'status': 'PASS', 'command': cmd, 'output': result.stdout.strip(),
            'test_source_sha256': sha(path)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    ap.add_argument('--work', type=Path, required=True)
    args = ap.parse_args()
    root, work = args.root.resolve(), args.work.resolve()
    donor = root/'build/cedar-wc-v2-20261117'
    baseline = donor/'t736-cedar-wc-v2'
    sdk = Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    if work.exists() or root not in work.parents:
        ap.error('work must be a new directory inside the mainline workspace')
    assert sha(donor/'src/omx/gstomxvideodec.c') == BASE_SOURCE_SHA
    assert sha(donor/'src/omx/gstomxallocator.c') == BASE_ALLOCATOR_SHA
    assert sha(baseline/'manifest.json') == BASE_MANIFEST_SHA
    work.mkdir(parents=True)
    source, build = work/'src', work/'build'
    shutil.copytree(donor/'src', source)
    p = source/'omx/gstomxvideodec.c'
    old = p.read_text()
    changed = patch_decoder(old)
    p.write_text(changed)
    patch = work/'gstomx-validated-output-crop.patch'
    patch.write_text(''.join(difflib.unified_diff(old.splitlines(True), changed.splitlines(True),
                    'a/omx/gstomxvideodec.c', 'b/omx/gstomxvideodec.c')))
    tests = test_crop_validator(source, work)
    env = dict(os.environ, PATH=str(sdk/'bin') + os.pathsep + os.environ['PATH'])
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
    bundle = work/'t736-cedar-wc-v3'
    shutil.copytree(baseline, bundle)
    (bundle/'manifest.json').rename(bundle/'BASELINE-WC-V2-MANIFEST.json')
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
    assert sha(source/'omx/gstomxallocator.c') == BASE_ALLOCATOR_SHA
    assert sha(donor/'src/omx/gstomxvideodec.c') == BASE_SOURCE_SHA
    metadata = {
        'schema': 1, 'status': 'COMPILED_HARDWARE_PENDING', 'board_access': False,
        'change': 'query and validate OMX output crop before releasing each output; attach complete GstVideoCropMeta on all output paths',
        'baseline_manifest_sha256': BASE_MANIFEST_SHA,
        'source_before_sha256': BASE_SOURCE_SHA, 'source_after_sha256': sha(p),
        'patch_sha256': sha(patch), 'v2_allocator_sha256': BASE_ALLOCATOR_SHA,
        'plugin_before_sha256': sha(baseline/plugin_rel),
        'plugin_after_sha256': sha(bundle/plugin_rel),
        'unchanged_libraries': unchanged, 'commands': commands, 'tests': tests,
        'crop_source': 'OMX_IndexConfigCommonOutputCrop for each nonempty output before pool copy/release; no fixed crop dimensions',
        'output_paths': ['matched pool, including need_copy', 'unmatched pool, including need_copy',
                         'matched no-pool copy', 'unmatched no-pool copy'],
        'failure_policy': 'fail output negotiation on absent, invalid, or unpublishable crop; no guessed visible dimensions',
        'kernel_and_memadapter_unchanged': True, 'v2_fd_ownership_preserved': True,
    }
    (bundle/'CROP-MANIFEST.json').write_text(json.dumps(metadata, indent=2)+'\n')
    (bundle/'README.txt').write_text(
        'Private T736 Cedar WC-v3: WC-v2 with validated complete crop metadata.\n'
        'Every nonempty output queries OMX output crop before any pool copy/release.\n'
        'Storage dimensions/stride remain separate from visible crop; all output paths carry the rectangle.\n'
        'Missing/invalid crop fails negotiation; no hardcoded visible dimensions or bottom padding.\n'
        'V2 DMA fd ownership, vendor libraries, MemAdapter and kernel are unchanged.\n'
        'Requires experimental /dev/dma_heap/t736-ve; imported memory remains pinned until reboot.\n'
        'Contains proprietary libraries: do not publish archive. Source patch is separately reviewable.\n'
        'Compilation and host bounds tests do not establish target decode or browser acceptance.\n')
    manifest = {'schema': 1, 'kind': 't736-cedar-wc-v3', 'status': metadata['status'],
                'files': [{'path': q.relative_to(bundle).as_posix(), 'bytes': q.stat().st_size,
                           'sha256': sha(q)} for q in sorted(bundle.rglob('*')) if q.is_file()]}
    (bundle/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    archive = work/'t736-cedar-wc-v3.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    result = {'schema': 1, 'status': metadata['status'], 'package': str(archive),
              'bytes': archive.stat().st_size, 'sha256': sha(archive),
              'manifest_sha256': sha(bundle/'manifest.json'),
              'plugin_sha256': sha(bundle/plugin_rel), 'source_patch_sha256': sha(patch),
              'source_sha256': sha(p), 'tests': tests}
    (work/'build-record.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
