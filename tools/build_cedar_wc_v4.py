#!/usr/bin/env python3
"""Isolated WC-v4: broker-only idle action, preserving WC-v3 crop/fd fixes."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

BASE_SOURCE_SHA = 'b084e2e7c6ce2e2972ce3d4270a4084e775750930cfba26d03fc794148af2682'
BASE_ALLOCATOR_SHA = '19fceea8e5598e67af7ad9b70678a8db33aa0c995cf4689e77869efdaa459e72'
BASE_MANIFEST_SHA = '253edaf6e5f91f3ee9d5940fffa28933fa1ca0ff9f90abdadce6866c10eb077d'

IDLE_HANDLER = r'''/* Private broker action, only after a completed flushing seek and with no
 * further input being fed. This is deliberately NOT a Gst state mapping:
 * vendor Pause->Executing returns both port queues and is not a lossless
 * ordinary playback pause. Keep initial Gst PAUSED/preroll unchanged. */
static gboolean
gst_omx_video_dec_t736_set_idle (GstOMXVideoDec * self, gboolean idle)
{
  OMX_STATETYPE state, target;
  OMX_ERRORTYPE err;
  gboolean success = TRUE;
  const gchar *reason = NULL;

  GST_VIDEO_DECODER_STREAM_LOCK (self);
  if (!self->dec)
    goto done;
  state = gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
  if (state == OMX_StateLoaded || state == OMX_StateIdle)
    goto done;                 /* First input still initializes the component. */

  if (idle) {
    if (state == OMX_StatePause)
      goto done;
    if (state != OMX_StateExecuting || self->started) {
      reason = "idle requires a flushed, stopped Executing decoder";
      goto failed;
    }
    target = OMX_StatePause;
  } else {
    if (state == OMX_StateExecuting)
      goto done;
    if (state != OMX_StatePause) {
      reason = "resume requires a valid paused decoder";
      goto failed;
    }
    target = OMX_StateExecuting;
  }

  err = gst_omx_component_set_state (self->dec, target);
  if (err != OMX_ErrorNone) {
    reason = "OpenMAX rejected broker idle/resume transition";
    goto failed;
  }
  if (gst_omx_component_get_state (self->dec, 5 * GST_SECOND) != target) {
    reason = "OpenMAX broker idle/resume transition failed or timed out";
    goto failed;
  }
  if (!idle) {
    /* Tina Pause->Executing calls doFlushPortBuffer on both ports. Populate
     * drains the returned output queue and resets flags/nFilledLen before
     * FillThisBuffer, without reallocating the existing pool. */
    if (!self->dec_out_port ||
        gst_omx_port_populate (self->dec_out_port) != OMX_ErrorNone) {
      reason = "cannot repopulate the existing output pool after broker resume";
      goto failed;
    }
  }
  goto done;

failed:
  self->downstream_flow_ret = GST_FLOW_ERROR;
  success = FALSE;
done:
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  if (!success)
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED,
        ("T736 broker idle/resume failed"), ("%s", reason));
  return success;
}

'''


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def replace_once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError(f'Expected unique anchor: {old[:80]}')
    return s.replace(old, new, 1)


def patch_decoder(old):
    s = replace_once(old, 'static void\ngst_omx_video_dec_class_init (GstOMXVideoDecClass * klass)\n',
                     IDLE_HANDLER + 'static void\ngst_omx_video_dec_class_init (GstOMXVideoDecClass * klass)\n')
    s = replace_once(s, '  DEBUG_INIT\n\n  if (g_getenv ("GST_OMX_VIDEO_DEC_DEBUG")) {\n',
                     '''  DEBUG_INIT

  g_signal_new_class_handler ("t736-set-idle", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_omx_video_dec_t736_set_idle), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_BOOLEAN, 1, G_TYPE_BOOLEAN);

  if (g_getenv ("GST_OMX_VIDEO_DEC_DEBUG")) {
''')
    return s


def test_handler(source, work):
    actual = (source/'omx/gstomxvideodec.c').read_text()
    first = actual.index('static gboolean\ngst_omx_video_dec_t736_set_idle')
    last = actual.index('\nstatic void\ngst_omx_video_dec_class_init', first)
    test = r'''#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef int gboolean;
typedef char gchar;
typedef int OMX_STATETYPE;
typedef int OMX_ERRORTYPE;
enum {OMX_StateInvalid, OMX_StateLoaded, OMX_StateIdle, OMX_StateExecuting, OMX_StatePause};
enum {OMX_ErrorNone=0, GST_FLOW_ERROR=-5};
#define TRUE 1
#define FALSE 0
#define GST_SECOND UINT64_C(1000000000)
typedef struct { void *dec, *dec_out_port; int started, downstream_flow_ret; } GstOMXVideoDec;
static int state, set_fail, wait_fail, populate_fail, sets, populates, errors, locks;
#define GST_VIDEO_DECODER_STREAM_LOCK(s) do { (void)(s); locks++; } while(0)
#define GST_VIDEO_DECODER_STREAM_UNLOCK(s) do { (void)(s); locks--; } while(0)
#define GST_ELEMENT_ERROR(s,a,b,c,e) do { (void)(s); (void)reason; errors++; } while(0)
static int gst_omx_component_get_state(void *c, uint64_t timeout) {
  assert(c && timeout==5*GST_SECOND); return wait_fail ? OMX_StateInvalid : state;
}
static int gst_omx_component_set_state(void *c,int target) {
  assert(c); sets++; if(set_fail) return 1; state=target; return OMX_ErrorNone;
}
static int gst_omx_port_populate(void *p) {
  assert(p && state==OMX_StateExecuting); populates++; return populate_fail;
}
'''
    test += actual[first:last]
    test += r'''
static void reset(int s) { state=s; set_fail=wait_fail=populate_fail=sets=populates=errors=locks=0; }
int main(void) {
  GstOMXVideoDec d={0};
  reset(OMX_StateLoaded); assert(gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(!sets && !locks);
  d.dec=&d; d.dec_out_port=&d;
  reset(OMX_StateLoaded); assert(gst_omx_video_dec_t736_set_idle(&d,FALSE)); assert(!sets);
  reset(OMX_StateIdle); assert(gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(!sets);
  reset(OMX_StateExecuting); assert(gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(state==OMX_StatePause && sets==1 && !populates);
  assert(gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(sets==1);
  assert(gst_omx_video_dec_t736_set_idle(&d,FALSE)); assert(state==OMX_StateExecuting && sets==2 && populates==1);
  assert(gst_omx_video_dec_t736_set_idle(&d,FALSE)); assert(sets==2 && populates==1);
  reset(OMX_StateExecuting); d.started=TRUE; assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(!sets && errors==1 && !locks); d.started=FALSE;
  reset(OMX_StateExecuting); set_fail=1; assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(errors==1 && !locks);
  reset(OMX_StateExecuting); wait_fail=1; assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE)); assert(errors==1 && !locks);
  reset(OMX_StatePause); populate_fail=1; assert(!gst_omx_video_dec_t736_set_idle(&d,FALSE)); assert(errors==1 && populates==1 && !locks);
  reset(OMX_StatePause); d.dec_out_port=NULL; assert(!gst_omx_video_dec_t736_set_idle(&d,FALSE)); assert(errors==1 && !populates && !locks);
  puts("PASS: extracted idle action; initial/idempotent states, pause/resume/populate, active-input rejection, state/queue failures, balanced locks");
  return 0;
}
'''
    p = work/'idle-action-test.c'; p.write_text(test)
    cmd = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', str(p), '-o', str(work/'idle-action-test')]
    subprocess.run(cmd, check=True)
    run = subprocess.run([str(work/'idle-action-test')], capture_output=True, text=True, check=True)
    (work/'idle-action-test.log').write_text(run.stdout+run.stderr)
    return {'status':'PASS', 'command':cmd, 'output':run.stdout.strip(), 'source_sha256':sha(p)}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    ap.add_argument('--work', type=Path, required=True)
    args=ap.parse_args(); root=args.root.resolve(); work=args.work.resolve()
    donor=root/'build/cedar-wc-v3-20261117'; baseline=donor/'t736-cedar-wc-v3'
    sdk=Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    if work.exists() or root not in work.parents:
        ap.error('work must be a new directory inside mainline')
    assert sha(donor/'src/omx/gstomxvideodec.c')==BASE_SOURCE_SHA
    assert sha(donor/'src/omx/gstomxallocator.c')==BASE_ALLOCATOR_SHA
    assert sha(baseline/'manifest.json')==BASE_MANIFEST_SHA
    work.mkdir(parents=True); source=work/'src'; build=work/'build'
    shutil.copytree(donor/'src',source)
    p=source/'omx/gstomxvideodec.c'; old=p.read_text(); changed=patch_decoder(old); p.write_text(changed)
    patch=work/'gstomx-broker-idle.patch'
    patch.write_text(''.join(difflib.unified_diff(old.splitlines(True),changed.splitlines(True),
                    'a/omx/gstomxvideodec.c','b/omx/gstomxvideodec.c')))
    tests=test_handler(source,work)
    env=dict(os.environ,PATH=str(sdk/'bin')+os.pathsep+os.environ['PATH'])
    commands=[
        [str(sdk/'bin/meson'),'setup',str(build),str(source),'--cross-file',
         str(sdk/'etc/meson/cross-compilation.conf'),'--prefix=/usr','--libdir=lib',
         '-Dtarget=buildroot','-Dstruct_packing=8','-Dexamples=disabled',
         '-Dtests=disabled','-Dtools=disabled','-Ddoc=disabled'],
        [str(sdk/'bin/ninja'),'-C',str(build),'-j4']]
    for name,cmd in zip(('configure','build'),commands):
        with (work/(name+'.log')).open('w') as out:
            subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,env=env,check=True)
    bundle=work/'t736-cedar-wc-v4'; shutil.copytree(baseline,bundle)
    (bundle/'manifest.json').rename(bundle/'BASELINE-WC-V3-MANIFEST.json')
    plugin_rel='lib/gstreamer-1.0/libgstomx.so'
    shutil.copy2(build/'omx/libgstomx.so',bundle/plugin_rel)
    shutil.copy2(patch,bundle/'source'/patch.name)
    unchanged=[]
    for src in sorted((baseline/'lib').rglob('*')):
        if not src.is_file() or src.relative_to(baseline).as_posix()==plugin_rel: continue
        dst=bundle/src.relative_to(baseline); assert sha(src)==sha(dst)
        unchanged.append({'path':src.relative_to(baseline).as_posix(),'sha256':sha(src)})
    assert sha(source/'omx/gstomxallocator.c')==BASE_ALLOCATOR_SHA
    assert sha(donor/'src/omx/gstomxvideodec.c')==BASE_SOURCE_SHA
    metadata={'schema':1,'status':'COMPILED_HARDWARE_PENDING','board_access':False,
        'change':'broker-only t736-set-idle(boolean)->boolean action, bounded OMX Pause/Executing transitions and output repopulation',
        'baseline_manifest_sha256':BASE_MANIFEST_SHA,'source_before_sha256':BASE_SOURCE_SHA,
        'source_after_sha256':sha(p),'patch_sha256':sha(patch),'plugin_sha256':sha(bundle/plugin_rel),
        'allocator_sha256':BASE_ALLOCATOR_SHA,'unchanged_libraries':unchanged,'commands':commands,'tests':tests,
        'contract':'after completed flushing seek, externally stop input; ordinary Gst state changes/preroll are unchanged',
        'idle':'Executing->Pause only when started=false; Pause/Loaded/Idle/no component are idempotent',
        'resume':'Pause->Executing then populate existing output pool; Executing/Loaded/Idle/no component are idempotent',
        'waits':'each OMX state query uses 5*GST_SECOND; failure returns false and posts GST_ELEMENT_ERROR',
        'kernel_memadapter_vendor_unchanged':True,'v2_fd_and_v3_crop_preserved':True}
    (bundle/'IDLE-MANIFEST.json').write_text(json.dumps(metadata,indent=2)+'\n')
    (bundle/'README.txt').write_text(
        'Private T736 Cedar WC-v4: WC-v3 plus broker-only t736-set-idle action.\n'
        'Call only after flush completion with no new input; TRUE parks OMX in Pause, FALSE resumes and repopulates output.\n'
        'Not a generic playback pause: vendor resume returns queued buffers. Gst normal state transitions/preroll unchanged.\n'
        'No pool teardown/reallocation in this action. V2 fd ownership and V3 validated crop retained.\n'
        'Vendor libraries/MemAdapter/kernel unchanged; imports remain pinned until reboot.\n'
        'Contains proprietary libraries: do not publish archive. Target idle/resume acceptance is pending.\n')
    manifest={'schema':1,'kind':'t736-cedar-wc-v4','status':metadata['status'],
        'files':[{'path':q.relative_to(bundle).as_posix(),'bytes':q.stat().st_size,'sha256':sha(q)}
                 for q in sorted(bundle.rglob('*')) if q.is_file()]}
    (bundle/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    archive=work/'t736-cedar-wc-v4.tar.gz'
    with tarfile.open(archive,'w:gz') as tar: tar.add(bundle,arcname=bundle.name)
    result={'schema':1,'status':metadata['status'],'package':str(archive),'bytes':archive.stat().st_size,
        'sha256':sha(archive),'plugin_sha256':sha(bundle/plugin_rel),'manifest_sha256':sha(bundle/'manifest.json'),
        'source_patch_sha256':sha(patch),'source_sha256':sha(p),'tests':tests}
    (work/'build-record.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result))


if __name__=='__main__': main()
