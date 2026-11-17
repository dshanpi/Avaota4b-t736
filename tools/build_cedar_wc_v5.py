#!/usr/bin/env python3
"""Isolated WC-v5: control-path diagnostics only, preserving WC-v4 decisions."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

BASE_SOURCE_SHA = '0c59ac64ef759bd856d309b15b39e64acfb91a6530c02b576f04f23f729e08fe'
BASE_ALLOCATOR_SHA = '19fceea8e5598e67af7ad9b70678a8db33aa0c995cf4689e77869efdaa459e72'
BASE_MANIFEST_SHA = 'eaf5fb8d56c1ee96b185ab8f0f7a88704ed8825347e58ad014278dfe2c6e466a'
BASE_PLUGIN_SHA = 'e92446f586b9b7ed00e28d2cafc76b12c95bd42efdc1428b52b0203cf480df17'

DIAGNOSTICS = r'''/* Diagnostic snapshots are taken with the decoder stream lock held.
 * The component mutex protects its raw state and both port queues. Never call
 * an OMX helper or print while holding that mutex: helpers acquire it too.
 * These diagnostics do not dispatch messages, change state, or retry errors. */
typedef struct {
  gboolean present, enabled, populated, flushing, flushed;
  gboolean enabled_pending, disabled_pending;
  guint index, allocated, pending;
  gint settings, configured;
} T736OmxPortSnapshot;

typedef struct {
  gboolean present, started;
  gint flow, raw, pending;
  guint error;
  T736OmxPortSnapshot in, out;
} T736OmxSnapshot;

static void
t736_omx_snapshot_port (GstOMXPort * port, T736OmxPortSnapshot * dst)
{
  if (!port)
    return;
  dst->present = TRUE;
  dst->index = port->index;
  dst->enabled = port->port_def.bEnabled;
  dst->populated = port->port_def.bPopulated;
  dst->flushing = port->flushing;
  dst->flushed = port->flushed;
  dst->enabled_pending = port->enabled_pending;
  dst->disabled_pending = port->disabled_pending;
  dst->allocated = port->buffers ? port->buffers->len : 0;
  dst->pending = g_queue_get_length (&port->pending_buffers);
  dst->settings = port->settings_cookie;
  dst->configured = port->configured_settings_cookie;
}

static void
t736_omx_snapshot (GstOMXVideoDec * self, T736OmxSnapshot * dst)
{
  memset (dst, 0, sizeof (*dst));
  dst->raw = dst->pending = -1;
  dst->started = self->started;
  dst->flow = self->downstream_flow_ret;
  if (!self->dec)
    return;
  g_mutex_lock (&self->dec->lock);
  dst->present = TRUE;
  dst->raw = self->dec->state;
  dst->pending = self->dec->pending_state;
  dst->error = self->dec->last_error;
  t736_omx_snapshot_port (self->dec_in_port, &dst->in);
  t736_omx_snapshot_port (self->dec_out_port, &dst->out);
  g_mutex_unlock (&self->dec->lock);
}

static void
t736_omx_print (GstOMXVideoDec * self, const gchar * phase,
    const T736OmxSnapshot * s, gint idle, gint actual, gint target,
    guint result, const gchar * reason)
{
  g_printerr ("T736_OMX_DIAG self=%p phase=%s idle=%d actual=%d target=%d "
      "result=0x%08x present=%d started=%d flow=%d raw=%d pending=%d "
      "error=0x%08x "
      "in={present:%d,index:%u,enabled:%d,populated:%d,flushing:%d,flushed:%d,"
      "enable_pending:%d,disable_pending:%d,allocated:%u,pending:%u,settings:%d,configured:%d} "
      "out={present:%d,index:%u,enabled:%d,populated:%d,flushing:%d,flushed:%d,"
      "enable_pending:%d,disable_pending:%d,allocated:%u,pending:%u,settings:%d,configured:%d} "
      "reason=%s\n", (void *) self, phase, idle, actual, target, result,
      s->present, s->started, s->flow, s->raw, s->pending, s->error,
      s->in.present, s->in.index, s->in.enabled, s->in.populated,
      s->in.flushing, s->in.flushed, s->in.enabled_pending,
      s->in.disabled_pending, s->in.allocated, s->in.pending,
      s->in.settings, s->in.configured,
      s->out.present, s->out.index, s->out.enabled, s->out.populated,
      s->out.flushing, s->out.flushed, s->out.enabled_pending,
      s->out.disabled_pending, s->out.allocated, s->out.pending,
      s->out.settings, s->out.configured, reason ? reason : "-");
}

static void
t736_omx_trace (GstOMXVideoDec * self, const gchar * phase,
    gint actual, gint target, guint result)
{
  T736OmxSnapshot snapshot;
  t736_omx_snapshot (self, &snapshot);
  t736_omx_print (self, phase, &snapshot, -1, actual, target, result, NULL);
}

static OMX_STATETYPE
t736_omx_diag_get_state (GstOMXVideoDec * self, GstClockTime timeout,
    const gchar * phase)
{
  OMX_STATETYPE state = gst_omx_component_get_state (self->dec, timeout);
  t736_omx_trace (self, phase, state, -1, 0);
  return state;
}

static OMX_ERRORTYPE
t736_omx_diag_set_state (GstOMXVideoDec * self, OMX_STATETYPE target,
    const gchar * phase)
{
  OMX_ERRORTYPE err = gst_omx_component_set_state (self->dec, target);
  t736_omx_trace (self, phase, -1, target, err);
  return err;
}

static OMX_ERRORTYPE
t736_omx_diag_set_flushing (GstOMXVideoDec * self, GstOMXPort * port,
    GstClockTime timeout, gboolean flush, const gchar * phase)
{
  OMX_ERRORTYPE err = gst_omx_port_set_flushing (port, timeout, flush);
  t736_omx_trace (self, phase, -1, -1, err);
  return err;
}

static OMX_ERRORTYPE
t736_omx_diag_populate (GstOMXVideoDec * self, GstOMXPort * port,
    const gchar * phase)
{
  OMX_ERRORTYPE err = gst_omx_port_populate (port);
  t736_omx_trace (self, phase, -1, -1, err);
  return err;
}

'''


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def replace_once(s, before, after):
    if s.count(before) != 1:
        raise RuntimeError(f'Expected unique anchor: {before[:100]}')
    return s.replace(before, after, 1)


def patch_decoder(old):
    first = old.index('/* Private broker action, only after')
    last = old.index('static void\ngst_omx_video_dec_class_init', first)
    action = old[first:last]
    action = replace_once(action, '  OMX_STATETYPE state, target;\n  OMX_ERRORTYPE err;',
        '  OMX_STATETYPE state = OMX_StateInvalid, target = OMX_StateInvalid;\n'
        '  OMX_ERRORTYPE err = OMX_ErrorNone;\n  T736OmxSnapshot snapshot;')
    action = replace_once(action, '  GST_VIDEO_DECODER_STREAM_LOCK (self);\n',
        '  GST_VIDEO_DECODER_STREAM_LOCK (self);\n'
        '  t736_omx_snapshot (self, &snapshot);\n'
        '  t736_omx_print (self, "action.entry", &snapshot, idle, -1, -1, 0, NULL);\n')
    action = replace_once(action, 'state = gst_omx_component_get_state (self->dec, 5 * GST_SECOND);',
        'state = t736_omx_diag_get_state (self, 5 * GST_SECOND, "action.initial-state");')
    action = replace_once(action, 'err = gst_omx_component_set_state (self->dec, target);',
        'err = t736_omx_diag_set_state (self, target, "action.set-state");')
    action = replace_once(action, '  if (gst_omx_component_get_state (self->dec, 5 * GST_SECOND) != target) {',
        '  state = t736_omx_diag_get_state (self, 5 * GST_SECOND, "action.wait-state");\n'
        '  if (state != target) {')
    action = replace_once(action, 'gst_omx_port_populate (self->dec_out_port) != OMX_ErrorNone',
        '(err = t736_omx_diag_populate (self, self->dec_out_port,\n'
        '                "action.populate-output")) != OMX_ErrorNone')
    action = replace_once(action, 'done:\n  GST_VIDEO_DECODER_STREAM_UNLOCK (self);',
        'done:\n  t736_omx_snapshot (self, &snapshot);\n'
        '  t736_omx_print (self, success ? "action.success" : "action.failure",\n'
        '      &snapshot, idle, state, target, err, reason);\n'
        '  GST_VIDEO_DECODER_STREAM_UNLOCK (self);')
    action = replace_once(action, '("T736 broker idle/resume failed"), ("%s", reason)',
        '("T736 broker idle/resume failed: %s; actual=%d started=%d "\n'
        '            "raw=%d pending=%d error=0x%08x result=0x%08x",\n'
        '            reason, (gint) state, snapshot.started, snapshot.raw,\n'
        '            snapshot.pending, snapshot.error, (guint) err),\n'
        '        ("idle=%d target=%d flow=%d", idle, (gint) target, snapshot.flow)')
    s = old[:first] + DIAGNOSTICS + action + old[last:]

    first = s.index('static gboolean\ngst_omx_video_dec_flush (GstVideoDecoder * decoder)\n')
    last = s.index('static GstFlowReturn\ngst_omx_video_dec_handle_frame', first)
    flush = s[first:last]
    flush = replace_once(flush, '  GST_DEBUG_OBJECT (self, "Flushing decoder");',
        '  GST_DEBUG_OBJECT (self, "Flushing decoder");\n'
        '  t736_omx_trace (self, "flush.entry", -1, -1, 0);')
    flush = replace_once(flush,
        '  if (gst_omx_component_get_state (self->dec, 0) == OMX_StateLoaded)\n    return TRUE;',
        '  if (t736_omx_diag_get_state (self, 0, "flush.initial-state") == OMX_StateLoaded) {\n'
        '    t736_omx_trace (self, "flush.loaded-return", OMX_StateLoaded, -1, 0);\n'
        '    return TRUE;\n  }')
    flush = replace_once(flush,
        'if (gst_omx_component_get_state (self->dec, 0) == OMX_StateExecuting)',
        'if (t736_omx_diag_get_state (self, 0, "flush.pause-check") == OMX_StateExecuting)')
    flush = replace_once(flush, 'gst_omx_component_set_state (self->dec, OMX_StatePause);',
        't736_omx_diag_set_state (self, OMX_StatePause, "flush.set-pause");')
    # Preserve the donor's unbounded waits; this candidate only observes them.
    wait = 'gst_omx_component_get_state (self->dec, GST_CLOCK_TIME_NONE);'
    assert flush.count(wait) == 2
    flush = flush.replace(wait,
        't736_omx_diag_get_state (self, GST_CLOCK_TIME_NONE, "flush.wait-pause");', 1)
    flush = flush.replace(wait,
        't736_omx_diag_get_state (self, GST_CLOCK_TIME_NONE, "flush.wait-executing");', 1)
    flush = replace_once(flush, 'gst_omx_component_set_state (self->dec, OMX_StateExecuting);',
        't736_omx_diag_set_state (self, OMX_StateExecuting, "flush.set-executing");')
    for port in ('in', 'out'):
        for value, label in (('TRUE', 'begin'), ('FALSE', 'end')):
            flush = replace_once(flush,
                f'gst_omx_port_set_flushing (self->dec_{port}_port, 5 * GST_SECOND, {value});',
                f't736_omx_diag_set_flushing (self, self->dec_{port}_port, 5 * GST_SECOND,\n'
                f'      {value}, "flush.{port}-{label}");')
    populate = 'gst_omx_port_populate (self->dec_out_port)'
    assert flush.count(populate) == 2  # ordinary build and RPI non-EGL branch
    flush = flush.replace(populate,
        't736_omx_diag_populate (self, self->dec_out_port, "flush.populate-output")')
    flush = replace_once(flush, '  GST_DEBUG_OBJECT (self, "Flush finished");',
        '  GST_DEBUG_OBJECT (self, "Flush finished");\n'
        '  t736_omx_trace (self, "flush.end", -1, -1, err);')
    return s[:first] + flush + s[last:]


def test_handler(source, work):
    actual = (source / 'omx/gstomxvideodec.c').read_text()
    first = actual.index('/* Diagnostic snapshots are taken')
    last = actual.index('static void\ngst_omx_video_dec_class_init', first)
    test = r'''#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
typedef int gboolean;
typedef char gchar;
typedef int gint;
typedef unsigned guint;
typedef uint64_t GstClockTime;
typedef int OMX_STATETYPE;
typedef unsigned OMX_ERRORTYPE;
enum {OMX_StateInvalid, OMX_StateLoaded, OMX_StateIdle, OMX_StateExecuting, OMX_StatePause};
enum {OMX_ErrorNone=0, GST_FLOW_ERROR=-5};
#define TRUE 1
#define FALSE 0
#define GST_SECOND UINT64_C(1000000000)
typedef struct { int locked; } GMutex;
typedef struct { guint len; } GPtrArray;
typedef struct { guint length; } GQueue;
typedef struct { gboolean bEnabled, bPopulated; } PortDefinition;
typedef struct { GMutex lock; OMX_STATETYPE state,pending_state; OMX_ERRORTYPE last_error; } GstOMXComponent;
typedef struct { GstOMXComponent *comp; guint index; PortDefinition port_def;
 GPtrArray *buffers; GQueue pending_buffers; gboolean flushing,flushed,enabled_pending,disabled_pending;
 gint settings_cookie,configured_settings_cookie; } GstOMXPort;
typedef struct { GstOMXComponent *dec; GstOMXPort *dec_in_port,*dec_out_port;
 gboolean started; gint downstream_flow_ret; } GstOMXVideoDec;
static GstOMXComponent comp;
static GstOMXPort inport,outport;
static GPtrArray inbuffers={4},outbuffers={8};
static int set_fail,wait_fail,populate_fail,flushing_fail,sets,populates,errors,locks,prints;
static char last_print[4096],last_error[1024];
static void g_mutex_lock(GMutex *m) { assert(!m->locked);m->locked=1; }
static void g_mutex_unlock(GMutex *m) { assert(m->locked);m->locked=0; }
static guint g_queue_get_length(GQueue *q) { assert(comp.lock.locked);return q->length; }
static void g_printerr(const char *fmt,...) {
 va_list a;assert(!comp.lock.locked);prints++;va_start(a,fmt);
 vsnprintf(last_print,sizeof(last_print),fmt,a);va_end(a);
}
static void capture_error(const char *fmt,...) {
 va_list a;assert(!comp.lock.locked && !locks);va_start(a,fmt);
 vsnprintf(last_error,sizeof(last_error),fmt,a);va_end(a);
}
#define GST_VIDEO_DECODER_STREAM_LOCK(s) do { (void)(s);assert(!locks);locks++; } while(0)
#define GST_VIDEO_DECODER_STREAM_UNLOCK(s) do { (void)(s);assert(locks==1);locks--; } while(0)
#define GST_ELEMENT_ERROR(s,a,b,c,e) do { (void)(s);errors++;capture_error c; } while(0)
static OMX_STATETYPE gst_omx_component_get_state(GstOMXComponent *c,GstClockTime t) {
 assert(c && !c->lock.locked && (t==0 || t==5*GST_SECOND || t==UINT64_MAX));
 return wait_fail || c->last_error ? OMX_StateInvalid : c->state;
}
static OMX_ERRORTYPE gst_omx_component_set_state(GstOMXComponent *c,OMX_STATETYPE target) {
 assert(c && !c->lock.locked);sets++;if(set_fail)return 0x80001001u;c->state=target;return 0;
}
static OMX_ERRORTYPE gst_omx_port_populate(GstOMXPort *p) {
 assert(p && !p->comp->lock.locked);populates++;return populate_fail ? 0x80001018u : 0;
}
static OMX_ERRORTYPE gst_omx_port_set_flushing(GstOMXPort *p,GstClockTime t,gboolean f) {
 assert(p && !p->comp->lock.locked && t==5*GST_SECOND);
 if(flushing_fail)return 0x80001011u;
 p->flushing=f;return 0;
}
'''
    test += actual[first:last]
    test += r'''
static void reset(GstOMXVideoDec *d,int state) {
 memset(&comp,0,sizeof(comp));comp.state=state;
 memset(&inport,0,sizeof(inport));memset(&outport,0,sizeof(outport));
 inport.comp=outport.comp=&comp;inport.index=0;outport.index=1;
 inport.port_def.bEnabled=outport.port_def.bEnabled=TRUE;
 inport.port_def.bPopulated=TRUE;outport.port_def.bPopulated=FALSE;
 inport.buffers=&inbuffers;outport.buffers=&outbuffers;
 inport.pending_buffers.length=2;outport.pending_buffers.length=7;
 outport.settings_cookie=5;outport.configured_settings_cookie=4;
 *d=(GstOMXVideoDec){&comp,&inport,&outport,FALSE,0};
 set_fail=wait_fail=populate_fail=flushing_fail=sets=populates=errors=locks=prints=0;
 last_print[0]=last_error[0]=0;
}
int main(void) {
 GstOMXVideoDec d;T736OmxSnapshot s;
 reset(&d,OMX_StateExecuting);comp.pending_state=OMX_StatePause;comp.last_error=0x80001001u;
 d.started=TRUE;outport.flushing=TRUE;outport.disabled_pending=TRUE;
 t736_omx_snapshot(&d,&s);
 assert(s.present && s.started && s.raw==OMX_StateExecuting && s.pending==OMX_StatePause);
 assert(s.error==0x80001001u && s.in.present && s.in.allocated==4 && s.in.pending==2);
 assert(s.out.present && !s.out.populated && s.out.flushing && s.out.disabled_pending);
 assert(s.out.allocated==8 && s.out.pending==7 && s.out.settings==5 && s.out.configured==4);
 comp.state=OMX_StateIdle;outport.pending_buffers.length=0;assert(s.raw==OMX_StateExecuting && s.out.pending==7);
 d.dec=NULL;t736_omx_snapshot(&d,&s);assert(!s.present && s.raw==-1 && !s.in.present && !s.out.present);
 assert(gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(!sets && !locks && prints==2);
 reset(&d,OMX_StateLoaded);assert(gst_omx_video_dec_t736_set_idle(&d,FALSE));assert(!sets);
 reset(&d,OMX_StateIdle);assert(gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(!sets);
 reset(&d,OMX_StateExecuting);assert(gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(comp.state==OMX_StatePause && sets==1 && !populates);
 assert(gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(sets==1);
 assert(gst_omx_video_dec_t736_set_idle(&d,FALSE));assert(comp.state==OMX_StateExecuting && sets==2 && populates==1);
 assert(gst_omx_video_dec_t736_set_idle(&d,FALSE));assert(sets==2 && populates==1);
 reset(&d,OMX_StateExecuting);d.started=TRUE;assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE));
 assert(!sets && errors==1 && !locks && d.downstream_flow_ret==GST_FLOW_ERROR);
 assert(strstr(last_error,"idle requires") && strstr(last_error,"actual=3 started=1 raw=3 pending=0 error=0x00000000"));
 assert(strstr(last_print,"action.failure") && strstr(last_print,"allocated:8,pending:7"));
 reset(&d,OMX_StateExecuting);set_fail=1;assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(strstr(last_error,"result=0x80001001"));
 reset(&d,OMX_StateExecuting);wait_fail=1;comp.pending_state=OMX_StatePause;
 assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(strstr(last_error,"actual=0 started=0 raw=3 pending=4"));
 reset(&d,OMX_StateExecuting);comp.last_error=0x80001001u;
 assert(!gst_omx_video_dec_t736_set_idle(&d,TRUE));assert(strstr(last_error,"error=0x80001001"));
 reset(&d,OMX_StatePause);populate_fail=1;assert(!gst_omx_video_dec_t736_set_idle(&d,FALSE));assert(errors==1 && populates==1 && !locks);
 reset(&d,OMX_StatePause);d.dec_out_port=NULL;assert(!gst_omx_video_dec_t736_set_idle(&d,FALSE));assert(!populates && strstr(last_print,"out={present:0"));
 reset(&d,OMX_StateExecuting);flushing_fail=1;
 assert(t736_omx_diag_set_flushing(&d,&outport,5*GST_SECOND,TRUE,"flush.out-begin")==0x80001011u);
 assert(strstr(last_print,"result=0x80001011") && !outport.flushing);
 flushing_fail=0;assert(!t736_omx_diag_set_flushing(&d,&outport,5*GST_SECOND,TRUE,"flush.out-begin"));assert(outport.flushing);
 assert(!comp.lock.locked && !locks);
 puts("PASS: extracted snapshot/action; raw vs queried state, ports/queues, unlocked APIs/printing, unchanged idle gates/failures, detailed primary error");
 return 0;
}
'''
    p = work / 'state-diagnostics-test.c'
    p.write_text(test)
    cmd = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           str(p), '-o', str(work / 'state-diagnostics-test')]
    subprocess.run(cmd, check=True)
    run = subprocess.run([str(work / 'state-diagnostics-test')],
                         capture_output=True, text=True, check=True)
    (work / 'state-diagnostics-test.log').write_text(run.stdout + run.stderr)
    return {'status': 'PASS', 'command': cmd, 'output': run.stdout.strip(),
            'source_sha256': sha(p)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path('/home/ubuntu/t736-mainline'))
    ap.add_argument('--work', type=Path, required=True)
    args = ap.parse_args()
    root, work = args.root.resolve(), args.work.resolve()
    donor = root / 'build/cedar-wc-v4-20261117'
    baseline = donor / 't736-cedar-wc-v4'
    sdk = Path('/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host')
    if work.exists() or root not in work.parents:
        ap.error('work must be a new directory inside mainline')
    assert sha(donor / 'src/omx/gstomxvideodec.c') == BASE_SOURCE_SHA
    assert sha(donor / 'src/omx/gstomxallocator.c') == BASE_ALLOCATOR_SHA
    assert sha(baseline / 'manifest.json') == BASE_MANIFEST_SHA
    plugin_rel = 'lib/gstreamer-1.0/libgstomx.so'
    assert sha(baseline / plugin_rel) == BASE_PLUGIN_SHA
    work.mkdir(parents=True)
    source, build = work / 'src', work / 'build'
    shutil.copytree(donor / 'src', source)
    p = source / 'omx/gstomxvideodec.c'
    old = p.read_text()
    changed = patch_decoder(old)
    p.write_text(changed)
    patch = work / 'gstomx-broker-state-diagnostics.patch'
    patch.write_text(''.join(difflib.unified_diff(old.splitlines(True),
        changed.splitlines(True), 'a/omx/gstomxvideodec.c', 'b/omx/gstomxvideodec.c')))
    tests = test_handler(source, work)
    env = dict(os.environ, PATH=str(sdk / 'bin') + os.pathsep + os.environ['PATH'])
    commands = [[str(sdk / 'bin/meson'), 'setup', str(build), str(source),
                 '--cross-file', str(sdk / 'etc/meson/cross-compilation.conf'),
                 '--prefix=/usr', '--libdir=lib', '-Dtarget=buildroot',
                 '-Dstruct_packing=8', '-Dexamples=disabled', '-Dtests=disabled',
                 '-Dtools=disabled', '-Ddoc=disabled'],
                [str(sdk / 'bin/ninja'), '-C', str(build), '-j4']]
    for name, cmd in zip(('configure', 'build'), commands):
        with (work / (name + '.log')).open('w') as out:
            subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT,
                           env=env, check=True)
    bundle = work / 't736-cedar-wc-v5'
    shutil.copytree(baseline, bundle)
    (bundle / 'manifest.json').rename(bundle / 'BASELINE-WC-V4-MANIFEST.json')
    shutil.copy2(build / 'omx/libgstomx.so', bundle / plugin_rel)
    shutil.copy2(patch, bundle / 'source' / patch.name)
    unchanged = []
    for src in sorted((baseline / 'lib').rglob('*')):
        if not src.is_file() or src.relative_to(baseline).as_posix() == plugin_rel:
            continue
        dst = bundle / src.relative_to(baseline)
        assert sha(src) == sha(dst)
        unchanged.append({'path': src.relative_to(baseline).as_posix(), 'sha256': sha(src)})
    assert sha(source / 'omx/gstomxallocator.c') == BASE_ALLOCATOR_SHA
    assert sha(donor / 'src/omx/gstomxvideodec.c') == BASE_SOURCE_SHA
    assert sha(baseline / 'manifest.json') == BASE_MANIFEST_SHA
    assert sha(baseline / plugin_rel) == BASE_PLUGIN_SHA
    metadata = {'schema': 1, 'status': 'COMPILED_DIAGNOSTIC_CANDIDATE_HARDWARE_PENDING',
        'board_access': False, 'change': 'control-path snapshots and detailed idle failure messages only',
        'baseline_manifest_sha256': BASE_MANIFEST_SHA, 'source_before_sha256': BASE_SOURCE_SHA,
        'source_after_sha256': sha(p), 'patch_sha256': sha(patch),
        'plugin_sha256': sha(bundle / plugin_rel), 'allocator_sha256': BASE_ALLOCATOR_SHA,
        'unchanged_libraries': unchanged, 'commands': commands, 'tests': tests,
        'locking': 'decoder stream lock; snapshot raw component/port fields under component mutex; all API calls and printing outside component mutex',
        'logging': 'g_printerr only on idle/resume actions and actual decoder flush control path; no per-frame logging',
        'preserved': ['idle/resume state and started gates', 'poison/error return policy',
            'donor flush return/error handling and unbounded waits', 'no retries',
            'v2 DMA fd ownership', 'v3 crop metadata', 'v4 idle semantics',
            'vendor libraries', 'kernel', 'MemAdapter', 'pinned buffer lifetime'],
        'limits': 'Diagnostic only. Existing flush may ignore OMX errors and return TRUE. No recovery change or hardware acceptance.'}
    (bundle / 'STATE-DIAGNOSTICS-MANIFEST.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (bundle / 'README.txt').write_text(
        'Private T736 Cedar WC-v5 diagnostic candidate, based on frozen WC-v4.\n'
        'Only control-path snapshots and detailed primary idle errors change.\n'
        'No recovery, state gate, input, allocator, vendor library or kernel changes.\n'
        'Original donor flush error handling and waits remain unchanged.\n'
        'Not deployed or hardware accepted. Contains proprietary libraries: do not publish archive.\n')
    manifest = {'schema': 1, 'kind': 't736-cedar-wc-v5', 'status': metadata['status'],
        'files': [{'path': q.relative_to(bundle).as_posix(), 'bytes': q.stat().st_size,
                   'sha256': sha(q)} for q in sorted(bundle.rglob('*')) if q.is_file()]}
    (bundle / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    archive = work / 't736-cedar-wc-v5.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    result = {'schema': 1, 'status': metadata['status'], 'package': str(archive),
        'bytes': archive.stat().st_size, 'sha256': sha(archive),
        'plugin_sha256': sha(bundle / plugin_rel), 'manifest_sha256': sha(bundle / 'manifest.json'),
        'source_patch_sha256': sha(patch), 'source_sha256': sha(p), 'tests': tests}
    (work / 'build-record.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
