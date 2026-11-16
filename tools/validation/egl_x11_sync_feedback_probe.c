/* Isolated X11/EGL sync feedback observation. One 320x240 static test window;
 * no modesetting, DRM master, compositor replacement or global configuration.
 * Usage: egl-sync-probe 0|1 (swap interval for this test surface only).
 * SBC counts completed swaps. UST/MSC is a retrace pair, NOT a timestamp
 * automatically attributable to the current submitted frame.
 * Compile: aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -std=c11 FILE -ldl -o OUT
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned EGLBoolean;
typedef int EGLint;
typedef unsigned long EGLNativeWindowType;
typedef void (*EGLProc)(void);
typedef EGLBoolean (*GetSyncValues)(EGLDisplay, EGLSurface, int64_t *, int64_t *, int64_t *);
typedef EGLBoolean (*GetMscRate)(EGLDisplay, EGLSurface, int32_t *, int32_t *);

enum { MAX_SAMPLES = 256, FRAME_LIMIT = 60 };
struct sample {
    const char *kind;
    int submitted;
    int64_t query_start_us, query_end_us, ust, msc, sbc;
    unsigned ok;
    int error;
};
struct swap {
    int submitted;
    int64_t start_us, end_us;
};
static struct sample samples[MAX_SAMPLES];
static struct swap swaps[FRAME_LIMIT];
static int sample_count, swap_count;

static int64_t monotonic_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static void sleep_us(long usec) {
    struct timespec delay = { usec / 1000000, (usec % 1000000) * 1000 };
    nanosleep(&delay, NULL);
}
static void *open_vendor(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/opt/t736-img-ddk-24.2-x11/lib/%s", name);
    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) { fprintf(stderr, "LOAD_FAILED %s: %s\n", path, dlerror()); exit(1); }
    return handle;
}
static void *symbol(void *handle, const char *name) {
    void *value = dlsym(handle, name);
    if (!value) { fprintf(stderr, "SYMBOL_MISSING %s\n", name); exit(1); }
    return value;
}
static int has_extension(const char *extensions, const char *name) {
    size_t length = strlen(name);
    const char *at = extensions;
    if (!at) return 0;
    while ((at = strstr(at, name))) {
        if ((at == extensions || at[-1] == ' ') && (at[length] == ' ' || !at[length])) return 1;
        at += length;
    }
    return 0;
}
static struct sample *observe(GetSyncValues get_values, EGLDisplay display,
                              EGLSurface surface, EGLint (*get_error)(void),
                              const char *kind, int submitted) {
    if (sample_count == MAX_SAMPLES) return NULL;
    struct sample *item = &samples[sample_count++];
    item->kind = kind;
    item->submitted = submitted;
    get_error();
    item->query_start_us = monotonic_us();
    item->ok = get_values(display, surface, &item->ust, &item->msc, &item->sbc);
    item->query_end_us = monotonic_us();
    item->error = get_error();
    return item;
}
#define FN(handle, ret, name, args) ret (*name) args = (ret (*) args)symbol(handle, #name)
#define REQUIRE(test, message) do { if (!(test)) { failure = message; goto cleanup; } } while (0)

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "0") && strcmp(argv[1], "1"))) {
        fprintf(stderr, "Usage: egl-sync-probe 0|1 (test-surface swap interval)\n");
        return 2;
    }
    if (!getenv("DISPLAY") || !*getenv("DISPLAY") ||
        !getenv("XAUTHORITY") || !*getenv("XAUTHORITY")) {
        fprintf(stderr, "Explicit DISPLAY and XAUTHORITY required\n");
        return 2;
    }
    alarm(8); /* Driver calls can stall; process exit also closes its X window. */
    int interval = argv[1][0] - '0';
    const char *failure = NULL;
    int pixel_verified = 0, extensions_present = 0, rate_ok = 0, rate_error = 0;
    int baseline_query_ok = 0;
    int32_t numerator = 0, denominator = 0;
    int64_t baseline_sbc = 0;
    int64_t started_us = monotonic_us(), ended_us;
    void *egl = open_vendor("libEGL.so.1");
    void *gles = open_vendor("libGLESv2.so.2");
    void *xlib = dlopen("libX11.so.6", RTLD_NOW);
    if (!xlib) { fprintf(stderr, "LOAD_FAILED libX11.so.6\n"); return 1; }
    FN(egl, EGLDisplay, eglGetDisplay, (void *));
    FN(egl, EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
    FN(egl, EGLBoolean, eglBindAPI, (unsigned));
    FN(egl, EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
    FN(egl, EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *));
    FN(egl, EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *));
    FN(egl, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
    FN(egl, EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
    FN(egl, EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface));
    FN(egl, EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint));
    FN(egl, EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext));
    FN(egl, EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface));
    FN(egl, EGLBoolean, eglTerminate, (EGLDisplay));
    FN(egl, EGLint, eglGetError, (void));
    FN(egl, const char *, eglQueryString, (EGLDisplay, EGLint));
    FN(egl, EGLProc, eglGetProcAddress, (const char *));
    FN(gles, const unsigned char *, glGetString, (unsigned));
    FN(gles, void, glEnable, (unsigned));
    FN(gles, void, glScissor, (int, int, int, int));
    FN(gles, void, glClearColor, (float, float, float, float));
    FN(gles, void, glClear, (unsigned));
    FN(gles, void, glReadPixels, (int, int, int, int, unsigned, unsigned, void *));
    FN(gles, unsigned, glGetError, (void));
    FN(xlib, void *, XOpenDisplay, (const char *));
    FN(xlib, int, XDefaultScreen, (void *));
    FN(xlib, void *, XDefaultVisual, (void *, int));
    FN(xlib, unsigned long, XVisualIDFromVisual, (void *));
    FN(xlib, unsigned long, XRootWindow, (void *, int));
    FN(xlib, unsigned long, XCreateSimpleWindow, (void *, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long));
    FN(xlib, int, XStoreName, (void *, unsigned long, const char *));
    FN(xlib, int, XMapWindow, (void *, unsigned long));
    FN(xlib, int, XSync, (void *, int));
    FN(xlib, int, XDestroyWindow, (void *, unsigned long));
    FN(xlib, int, XCloseDisplay, (void *));

    void *native = NULL;
    unsigned long window = 0, visual_id = 0;
    EGLDisplay display = NULL;
    EGLSurface surface = NULL;
    EGLContext context = NULL;
    EGLConfig config = NULL, configs[128];
    EGLint count = 0, major = 0, minor = 0;
    native = XOpenDisplay(NULL); REQUIRE(native, "XOpenDisplay");
    int screen = XDefaultScreen(native);
    visual_id = XVisualIDFromVisual(XDefaultVisual(native, screen));
    window = XCreateSimpleWindow(native, XRootWindow(native, screen), 20, 20, 320, 240, 0, 0, 0);
    REQUIRE(window, "XCreateSimpleWindow");
    XStoreName(native, window, "T736 EGL sync feedback probe");
    XMapWindow(native, window); XSync(native, 0);
    display = eglGetDisplay(native);
    REQUIRE(display && eglInitialize(display, &major, &minor), "eglInitialize");
    const char *extensions = eglQueryString(display, 0x3055);
    extensions_present = has_extension(extensions, "EGL_CHROMIUM_sync_control") &&
                         has_extension(extensions, "EGL_ANGLE_sync_control_rate");
    REQUIRE(extensions_present, "required sync extensions unavailable");
    GetSyncValues get_values = (GetSyncValues)eglGetProcAddress("eglGetSyncValuesCHROMIUM");
    GetMscRate get_rate = (GetMscRate)eglGetProcAddress("eglGetMscRateANGLE");
    REQUIRE(get_values && get_rate, "sync extension procedure unavailable");
    REQUIRE(eglBindAPI(0x30a0), "eglBindAPI ES");
    const EGLint attributes[] = {0x3033, 4, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038};
    REQUIRE(eglChooseConfig(display, attributes, configs, 128, &count), "eglChooseConfig");
    for (int i = 0; i < count; i++) {
        EGLint id = 0;
        eglGetConfigAttrib(display, configs[i], 0x302e, &id);
        if ((unsigned long)(unsigned)id == visual_id) { config = configs[i]; break; }
    }
    REQUIRE(config, "matching native visual");
    const EGLint context_attributes[] = {0x3098, 2, 0x3038};
    context = eglCreateContext(display, config, NULL, context_attributes);
    REQUIRE(context, "eglCreateContext");
    surface = eglCreateWindowSurface(display, config, window, NULL);
    REQUIRE(surface, "eglCreateWindowSurface");
    REQUIRE(eglMakeCurrent(display, surface, surface, context), "eglMakeCurrent");
    REQUIRE(eglSwapInterval(display, interval), "eglSwapInterval");
    const char *renderer = (const char *)glGetString(0x1f01);
    const char *vendor = (const char *)glGetString(0x1f00);
    REQUIRE(renderer && vendor && (strstr(vendor, "Imagination") || strstr(renderer, "PowerVR")), "PowerVR renderer identity");
    REQUIRE(!strstr(renderer, "llvmpipe") && !strstr(renderer, "softpipe"), "software renderer rejected");
    eglGetError(); rate_ok = get_rate(display, surface, &numerator, &denominator); rate_error = eglGetError();
    struct sample *initial = observe(get_values, display, surface, eglGetError, "baseline", 0);
    REQUIRE(initial, "sample capacity");
    baseline_query_ok = initial->ok;
    if (baseline_query_ok) baseline_sbc = initial->sbc;
    const unsigned char colors[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    glEnable(0x0c11);
    for (int frame = 0; frame < FRAME_LIMIT; frame++) {
        REQUIRE(monotonic_us() - started_us < 5000000, "five-second workload bound");
        observe(get_values, display, surface, eglGetError, "before_swap", swap_count);
        for (int q = 0; q < 4; q++) {
            glScissor((q % 2) * 160, (q / 2) * 120, 160, 120);
            glClearColor(colors[q][0] / 255.f, colors[q][1] / 255.f, colors[q][2] / 255.f, 1.f);
            glClear(0x4000);
        }
        if (frame == 0 || frame == FRAME_LIMIT - 1) {
            for (int q = 0; q < 4; q++) {
                unsigned char pixel[4] = {0};
                glReadPixels((q % 2) * 160 + 80, (q / 2) * 120 + 60, 1, 1, 0x1908, 0x1401, pixel);
                for (int c = 0; c < 3; c++) REQUIRE(abs((int)pixel[c] - colors[q][c]) <= 2, "GPU pixel readback");
            }
            pixel_verified++;
        }
        REQUIRE(glGetError() == 0, "GLES draw error");
        struct swap *item = &swaps[swap_count];
        item->start_us = monotonic_us();
        REQUIRE(eglSwapBuffers(display, surface), "eglSwapBuffers");
        item->end_us = monotonic_us();
        item->submitted = ++swap_count;
        observe(get_values, display, surface, eglGetError, "after_swap", swap_count);
        sleep_us(16000);
    }
    int64_t drain_deadline = monotonic_us() + 250000;
    do {
        struct sample *item = observe(get_values, display, surface, eglGetError, "drain", swap_count);
        if (baseline_query_ok && item && item->ok && item->sbc - baseline_sbc >= swap_count) break;
        sleep_us(5000);
    } while (monotonic_us() < drain_deadline);

cleanup:
    ended_us = monotonic_us();
    int last_error = eglGetError();
    if (display) {
        eglMakeCurrent(display, NULL, NULL, NULL);
        if (surface) eglDestroySurface(display, surface);
        if (context) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    if (native && window) XDestroyWindow(native, window);
    if (native) XCloseDisplay(native);
    alarm(0);
    printf("{\n\"schema\":\"t736-egl-x11-sync-feedback-v1\",\n\"status\":\"%s\",\n", failure ? "FAILED" : "OBSERVATION_COMPLETE");
    printf("\"failure\":%s%s%s,\n", failure ? "\"" : "", failure ? failure : "null", failure ? "\"" : "");
    printf("\"egl_error_at_end\":%d,\"egl_version\":\"%d.%d\",\"test_swap_interval\":%d,\n", last_error, major, minor, interval);
    printf("\"vendor_library_prefix\":\"/opt/t736-img-ddk-24.2-x11/lib\",\n");
    printf("\"required_extensions_present\":%s,\"rate_ok\":%s,\"rate_error\":%d,\"rate_numerator\":%d,\"rate_denominator\":%d,\n", extensions_present ? "true" : "false", rate_ok ? "true" : "false", rate_error, numerator, denominator);
    printf("\"elapsed_us\":%" PRId64 ",\"pixel_verified_frames\":%d,\"submitted_swaps\":%d,\"baseline_query_ok\":%s,\"baseline_sbc\":%" PRId64 ",\n", ended_us - started_us, pixel_verified, swap_count, baseline_query_ok ? "true" : "false", baseline_sbc);
    printf("\"scope\":\"Test surface only. SBC is completed swaps; UST/MSC is a retrace pair, not the current submitted frame timestamp. No physical-monitor validation.\",\n\"samples\":[\n");
    for (int i = 0; i < sample_count; i++) {
        struct sample *item = &samples[i];
        printf("%s{\"kind\":\"%s\",\"submitted_swaps\":%d,\"query_start_us\":%" PRId64 ",\"query_end_us\":%" PRId64 ",\"ok\":%s,\"egl_error\":%d,\"ust\":%" PRId64 ",\"msc\":%" PRId64 ",\"sbc\":%" PRId64 "}", i ? ",\n" : "", item->kind, item->submitted, item->query_start_us, item->query_end_us, item->ok ? "true" : "false", item->error, item->ust, item->msc, item->sbc);
    }
    printf("\n],\n\"swaps\":[\n");
    for (int i = 0; i < swap_count; i++) {
        struct swap *item = &swaps[i];
        printf("%s{\"submitted_swaps\":%d,\"swap_start_us\":%" PRId64 ",\"swap_return_us\":%" PRId64 "}", i ? ",\n" : "", item->submitted, item->start_us, item->end_us);
    }
    printf("\n]\n}\n");
    return failure ? 1 : 0;
}
