/* Compile against the candidate's extracted, unmodified tracker functions.
 * This is a mocked Cogl/EGL boundary test, not a Mutter build or board test.
 */
#include <glib.h>
#include <alloca.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned long Window;
typedef void *EGLSurface;
typedef unsigned EGLBoolean;
#define EGL_TRUE 1
typedef struct _CoglContext CoglContext;
typedef struct { CoglContext *context; GQueue frame_infos; } CoglOnscreenEgl;
typedef CoglOnscreenEgl CoglOnscreen;
typedef CoglOnscreenEgl CoglFramebuffer;
typedef int MtkRegion;
typedef struct {
  int64_t frame_counter, presentation_time_us;
  float refresh_rate;
  unsigned flags;
} CoglFrameInfo;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CoglFrameInfo, g_free)
typedef struct {
  void *edpy;
  gboolean (*pf_eglGetSyncValues)(void *, void *, gint64 *, gint64 *, gint64 *);
  gboolean (*pf_eglGetMscRate)(void *, void *, gint32 *, gint32 *);
} CoglRendererEGL;
typedef struct { void *winsys; } CoglRenderer;
typedef struct { CoglRenderer *renderer; } CoglDisplay;
struct _CoglContext { CoglDisplay *display; gboolean enabled; };
typedef struct {
  void (*swap_buffers_with_damage)(CoglOnscreen *, const MtkRegion *, CoglFrameInfo *, gpointer);
  void (*swap_region)(CoglOnscreen *, const MtkRegion *, CoglFrameInfo *, gpointer);
} CoglOnscreenClass;
typedef struct _CoglOnscreenXlib CoglOnscreenXlib;
static CoglOnscreenClass parent_class;
static gpointer cogl_onscreen_xlib_parent_class = &parent_class;
#define COGL_FRAMEBUFFER(value) ((CoglFramebuffer *)(value))
#define COGL_ONSCREEN(value) ((CoglOnscreen *)(value))
#define COGL_ONSCREEN_EGL(value) ((CoglOnscreenEgl *)(value))
#define COGL_ONSCREEN_XLIB(value) ((CoglOnscreenXlib *)(value))
#define COGL_ONSCREEN_CLASS(value) ((CoglOnscreenClass *)(value))
#define COGL_FRAMEBUFFER_STATE_BIND 1
#define COGL_WINSYS_FEATURE_SYNC_AND_COMPLETE_EVENT 1
#define COGL_FRAME_INFO_FLAG_SYMBOLIC 1
#define COGL_FRAME_INFO_FLAG_HW_CLOCK 2
#define COGL_FRAME_INFO_FLAG_ZERO_COPY 4
#define COGL_FRAME_INFO_FLAG_VSYNC 8
#define COGL_FRAME_EVENT_SYNC 1
#define COGL_FRAME_EVENT_COMPLETE 2

static gint64 now_us = 1000000;
static gint64 driver_sbc;
static gboolean query_ok, rate_ok, swap_ok;
static gint32 rate_numerator, rate_denominator;
static unsigned query_calls, parent_calls, region_calls;
typedef struct { int type; CoglFrameInfo info; } Event;
static Event events[128];
static unsigned event_count;
static unsigned trace_messages;
static void count_trace(const gchar *domain, GLogLevelFlags level,
                        const gchar *message, gpointer data) {
  (void)domain; (void)level; (void)data;
  assert(g_str_has_prefix(message, "EGL_X11_SBC ")); trace_messages++;
}
static guint fake_timer(guint interval, GSourceFunc callback, gpointer data) {
  (void)callback; (void)data; assert(interval == 2); return 1;
}
static void fake_source_name(guint id, const char *name) { (void)id; (void)name; }
#define g_timeout_add fake_timer
#define g_source_set_name_by_id fake_source_name
#define g_get_monotonic_time() now_us
static CoglContext *cogl_framebuffer_get_context(CoglFramebuffer *fb) { return fb->context; }
static void *cogl_onscreen_egl_get_egl_surface(CoglOnscreenEgl *onscreen) { return onscreen; }
static void cogl_context_flush_framebuffer_state(CoglContext *c, CoglFramebuffer *a,
                                                  CoglFramebuffer *b, int flags) {
  (void)c; (void)a; (void)b; assert(flags == COGL_FRAMEBUFFER_STATE_BIND);
}
static CoglFrameInfo *cogl_onscreen_pop_head_frame_info(CoglOnscreen *o) {
  return g_queue_pop_head(&o->frame_infos);
}
static gboolean cogl_context_has_winsys_feature(CoglContext *c, int f) {
  assert(f == COGL_WINSYS_FEATURE_SYNC_AND_COMPLETE_EVENT); return c->enabled;
}
static void _cogl_onscreen_queue_event(CoglOnscreen *o, int type, CoglFrameInfo *info) {
  (void)o; assert(event_count < G_N_ELEMENTS(events));
  events[event_count++] = (Event){type, *info};
}
typedef struct {
  void *egl_surface;
  EGLBoolean (*pf_eglSwapBuffersWithDamage)(void *, void *, const int *, int);
} CoglOnscreenEglPrivate;
static CoglOnscreenEglPrivate egl_private;
static unsigned plain_swap_calls, damage_swap_calls, flush_calls;
static CoglOnscreenEglPrivate *cogl_onscreen_egl_get_instance_private(CoglOnscreenEgl *o) {
  (void)o; return &egl_private;
}
static EGLBoolean eglSwapBuffers(void *d, void *s) {
  (void)d; (void)s; plain_swap_calls++; return swap_ok;
}
static EGLBoolean swap_damage(void *d, void *s, const int *r, int n) {
  (void)d; (void)s; (void)r; assert(n == 1); damage_swap_calls++; return swap_ok;
}
static int mtk_region_num_rectangles(const MtkRegion *r) { assert(r); return 1; }
static int cogl_framebuffer_get_height(CoglFramebuffer *f) { (void)f; return 240; }
static void cogl_region_to_flipped_array(const MtkRegion *r, int h, int *out) {
  (void)r; (void)h; for (int i = 0; i < 4; i++) out[i] = 0;
}
static void cogl_framebuffer_flush(CoglFramebuffer *f) { (void)f; flush_calls++; }
#define COGL_TRACE_BEGIN_SCOPED(name, text) do { (void)(text); } while (0)

#include "mutter_sbc_checked_swap_fragment.inc"

#include "mutter_sbc_tracker_fragment.inc"

static gboolean values(void *d, void *s, gint64 *ust, gint64 *msc, gint64 *sbc) {
  (void)d; (void)s; query_calls++; *ust = 999; *msc = 77; *sbc = driver_sbc; return query_ok;
}
static gboolean rate(void *d, void *s, gint32 *n, gint32 *q) {
  (void)d; (void)s; *n = rate_numerator; *q = rate_denominator; return rate_ok;
}
static void parent_swap(CoglOnscreen *o, const MtkRegion *r, CoglFrameInfo *i, gpointer d) {
  (void)o; (void)r; (void)i; (void)d; parent_calls++;
}
static void parent_region(CoglOnscreen *o, const MtkRegion *r, CoglFrameInfo *i, gpointer d) {
  (void)o; (void)r; (void)i; (void)d; region_calls++;
}
static CoglRendererEGL renderer_egl = {NULL, values, rate};
static CoglRenderer renderer = {&renderer_egl};
static CoglDisplay display = {&renderer};
static CoglContext context = {&display, TRUE};
static void reset(CoglOnscreenXlib *o) {
  *o = (CoglOnscreenXlib){0}; o->parent.context = &context;
  g_queue_init(&o->parent.frame_infos); g_queue_init(&o->sbc_pending);
  context.enabled = query_ok = rate_ok = swap_ok = TRUE;
  driver_sbc = 0; now_us = 1000000; rate_numerator = 60; rate_denominator = 1;
  query_calls = parent_calls = region_calls = event_count = 0;
  plain_swap_calls = damage_swap_calls = flush_calls = 0;
  egl_private = (CoglOnscreenEglPrivate){NULL, swap_damage};
  parent_class = (CoglOnscreenClass){parent_swap, parent_region};
}
static CoglFrameInfo *enqueue(CoglOnscreenXlib *o, int64_t counter) {
  CoglFrameInfo *i = g_new0(CoglFrameInfo, 1);
  i->frame_counter = counter; i->flags = 14; i->presentation_time_us = 123456;
  g_queue_push_tail(&o->parent.frame_infos, i); return i;
}
static void submit(CoglOnscreenXlib *o, int64_t counter) {
  CoglFrameInfo *i = enqueue(o, counter);
  cogl_onscreen_xlib_swap_buffers_with_damage(COGL_ONSCREEN(o), NULL, i, NULL);
}
static void assert_events(unsigned frames, gboolean symbolic, float expected_rate) {
  assert(event_count == frames * 2);
  for (unsigned i = 0; i < event_count; i++) {
    assert(events[i].type == (i % 2 ? COGL_FRAME_EVENT_COMPLETE : COGL_FRAME_EVENT_SYNC));
    assert(events[i].info.presentation_time_us == 0);
    assert((events[i].info.flags & 14) == 0);
    assert(!!(events[i].info.flags & 1) == symbolic);
    assert(events[i].info.refresh_rate == expected_rate);
  }
}
int main(void) {
  CoglOnscreenXlib o;
  unsigned cases = 0;
  g_unsetenv("COGL_EGL_X11_SBC_TRACE");

  reset(&o); submit(&o, 1); assert(event_count == 0);
  assert(poll_sbc(&o) == G_SOURCE_CONTINUE); assert(event_count == 0);
  driver_sbc = 1; assert(poll_sbc(&o) == G_SOURCE_REMOVE);
  assert_events(1, FALSE, 60); assert(o.sbc_confirmed == 1); cases++;

  reset(&o); submit(&o, 10); swap_ok = FALSE; submit(&o, 11);
  swap_ok = TRUE; driver_sbc = 1; submit(&o, 12);
  assert_events(3, TRUE, 0); assert(o.sbc_confirmed == 0 && o.sbc_submit_failures == 1); cases++;

  reset(&o); submit(&o, 10); submit(&o, 11); submit(&o, 12);
  driver_sbc = 3; poll_sbc(&o); assert_events(3, FALSE, 60);
  assert(events[0].info.frame_counter == 10 && events[4].info.frame_counter == 12); cases++;

  reset(&o); submit(&o, 1); query_ok = FALSE; poll_sbc(&o);
  assert_events(1, TRUE, 0); cases++;

  reset(&o); submit(&o, 1); driver_sbc = 1; poll_sbc(&o);
  event_count = 0; submit(&o, 2); driver_sbc = 0; poll_sbc(&o);
  assert_events(1, TRUE, 0); cases++;

  reset(&o); submit(&o, 1); driver_sbc = 2; poll_sbc(&o);
  assert_events(1, TRUE, 0); cases++;

  reset(&o); submit(&o, 1); now_us += 250001; poll_sbc(&o);
  assert_events(1, TRUE, 0); cases++;

  reset(&o); submit(&o, 1); rate_numerator = 50; driver_sbc = 1; poll_sbc(&o);
  assert_events(1, FALSE, 50); cases++;

  reset(&o); submit(&o, 1); rate_ok = FALSE; driver_sbc = 1; poll_sbc(&o);
  assert_events(1, FALSE, 0); cases++;

  reset(&o); submit(&o, 1);
  CoglFrameInfo *region_info = enqueue(&o, 2);
  cogl_onscreen_xlib_swap_region(COGL_ONSCREEN(&o), NULL, region_info, NULL);
  driver_sbc = 2; submit(&o, 3); assert_events(3, TRUE, 0);
  assert(o.sbc_confirmed == 0 && region_calls == 1); cases++;

  reset(&o); for (int i = 0; i < 17; i++) submit(&o, i);
  assert_events(17, TRUE, 0); assert(o.sbc_max_queue_depth == 17); cases++;

  reset(&o); submit(&o, 1); submit(&o, 2);
  ((CoglFrameInfo *)g_queue_peek_head(&o.parent.frame_infos))->frame_counter = 99;
  driver_sbc = 2; poll_sbc(&o); assert_events(2, TRUE, 0); cases++;

  reset(&o); rate_ok = FALSE; submit(&o, 1); assert_events(1, TRUE, 0); cases++;

  reset(&o); driver_sbc = G_MAXINT64; submit(&o, 1);
  assert_events(1, TRUE, 0); cases++;

  reset(&o); context.enabled = FALSE; submit(&o, 1);
  assert(parent_calls == 1 && query_calls == 0 && !o.sbc_attempted && event_count == 0);
  g_queue_clear_full(&o.parent.frame_infos, g_free); cases++;

  reset(&o); CoglFrameInfo dummy = {0}; MtkRegion damage = 1;
  assert(cogl_onscreen_egl_swap_buffers_with_damage_checked(COGL_ONSCREEN(&o), NULL, &dummy, NULL));
  assert(cogl_onscreen_egl_swap_buffers_with_damage_checked(COGL_ONSCREEN(&o), &damage, &dummy, NULL));
  swap_ok = FALSE;
  assert(!cogl_onscreen_egl_swap_buffers_with_damage_checked(COGL_ONSCREEN(&o), NULL, &dummy, NULL));
  assert(!cogl_onscreen_egl_swap_buffers_with_damage_checked(COGL_ONSCREEN(&o), &damage, &dummy, NULL));
  assert(plain_swap_calls == 2 && damage_swap_calls == 2 && flush_calls == 4); cases++;

  reset(&o); trace_messages = 0;
  guint log_handler = g_log_set_handler(NULL, G_LOG_LEVEL_MESSAGE, count_trace, NULL);
  g_setenv("COGL_EGL_X11_SBC_TRACE", "1", TRUE);
  for (int i = 1; i <= 1200; i++) {
    event_count = 0; submit(&o, i); driver_sbc = i; now_us += 100;
    poll_sbc(&o); assert_events(1, FALSE, 60);
  }
  assert(o.sbc_submissions == 1200 && o.sbc_confirmed == 1200 && o.sbc_symbolic == 0);
  assert(o.sbc_max_wait_us == 100 && o.sbc_max_queue_depth == 1);
  assert(trace_messages == 8 && o.sbc_trace_count == 8);
  trace_sbc_summary(&o, "dispose", TRUE); assert(trace_messages == 9);
  g_unsetenv("COGL_EGL_X11_SBC_TRACE"); g_log_remove_handler(NULL, log_handler); cases++;
  printf("MUTTER_SBC_TRACKER_MOCK_PASS cases=%u actual_candidate_functions=1 physical_display_test=0\n", cases);
  return 0;
}
