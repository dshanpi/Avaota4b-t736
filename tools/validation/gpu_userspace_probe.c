/* Explicit isolated IMG EGL/GLES smoke test. No modesetting or DRM master.
 * x11: maps a 320x240 test window; gbm: requires an explicit render node.
 * Both draw four colored regions, read back four pixels, and reject software.
 * The X11 screen itself and GNOME remain separate acceptance checks.
 * Runtime symbol binding avoids mixing target Mesa development headers/libs.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned EGLBoolean;
typedef int EGLint;
static int failed;
static void *open_lib(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/opt/t736-img-ddk-24.2-x11/lib/%s", name);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "LOAD_FAILED %s: %s\n", name, dlerror()); exit(1); }
    printf("LIBRARY %s\n", path);
    return h;
}
static void *sym(void *h, const char *name) {
    void *p = dlsym(h, name);
    if (!p) { fprintf(stderr, "SYMBOL_MISSING %s\n", name); exit(1); }
    return p;
}
#define FN(h, ret, name, args) ret (*name) args = (ret (*) args)sym(h, #name)
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr,"FAIL %s egl=0x%x\n",msg,eglGetError()); failed=1; goto cleanup; } } while(0)
int main(int argc, char **argv) {
    int is_x11 = argc == 2 && strcmp(argv[1], "x11") == 0;
    int is_gbm = argc == 3 && strcmp(argv[1], "gbm") == 0;
    if (!is_x11 && !is_gbm) { fprintf(stderr,"Usage: gpu-probe x11 | gbm /dev/dri/renderD<N>\n"); return 2; }
    if (is_gbm && strncmp(argv[2], "/dev/dri/renderD", 16) != 0) { fprintf(stderr,"Only an explicit DRM render node is accepted\n"); return 2; }
    void *egl = open_lib("libEGL.so.1");
    void *gles = open_lib("libGLESv2.so.2");
    FN(egl,EGLDisplay,eglGetDisplay,(void *));
    FN(egl,EGLBoolean,eglInitialize,(EGLDisplay,EGLint *,EGLint *));
    FN(egl,EGLBoolean,eglBindAPI,(unsigned));
    FN(egl,EGLBoolean,eglChooseConfig,(EGLDisplay,const EGLint *,EGLConfig *,EGLint,EGLint *));
    FN(egl,EGLBoolean,eglGetConfigAttrib,(EGLDisplay,EGLConfig,EGLint,EGLint *));
    FN(egl,EGLSurface,eglCreateWindowSurface,(EGLDisplay,EGLConfig,void *,const EGLint *));
    FN(egl,EGLContext,eglCreateContext,(EGLDisplay,EGLConfig,EGLContext,const EGLint *));
    FN(egl,EGLBoolean,eglMakeCurrent,(EGLDisplay,EGLSurface,EGLSurface,EGLContext));
    FN(egl,EGLBoolean,eglSwapBuffers,(EGLDisplay,EGLSurface));
    FN(egl,EGLBoolean,eglDestroyContext,(EGLDisplay,EGLContext));
    FN(egl,EGLBoolean,eglDestroySurface,(EGLDisplay,EGLSurface));
    FN(egl,EGLBoolean,eglTerminate,(EGLDisplay));
    FN(egl,EGLint,eglGetError,(void));
    FN(egl,const char *,eglQueryString,(EGLDisplay,EGLint));
    FN(gles,const unsigned char *,glGetString,(unsigned));
    FN(gles,void,glEnable,(unsigned)); FN(gles,void,glScissor,(int,int,int,int));
    FN(gles,void,glClearColor,(float,float,float,float)); FN(gles,void,glClear,(unsigned));
    FN(gles,void,glReadPixels,(int,int,int,int,unsigned,unsigned,void *));
    FN(gles,void,glFinish,(void)); FN(gles,unsigned,glGetError,(void));
    void *native = NULL, *gbm = NULL, *device = NULL, *gbm_surface = NULL;
    void *xlib = NULL; unsigned long window = 0, visual_id = 0; int fd = -1;
    EGLDisplay display = NULL; EGLSurface surface = NULL; EGLContext context = NULL;
    EGLConfig config = NULL, configs[128]; EGLint count = 0, major = 0, minor = 0;
    void (*gbm_surface_destroy)(void *) = NULL, (*gbm_device_destroy)(void *) = NULL;
    void *(*gbm_surface_lock_front_buffer)(void *) = NULL;
    void (*gbm_surface_release_buffer)(void *,void *) = NULL;
    int (*gbm_bo_get_fd)(void *) = NULL;
    int (*XDestroyWindow)(void *,unsigned long) = NULL, (*XCloseDisplay)(void *) = NULL;
    if (is_x11) {
        CHECK(getenv("DISPLAY") && getenv("XAUTHORITY"), "explicit DISPLAY and XAUTHORITY required");
        xlib = dlopen("libX11.so.6", RTLD_NOW); CHECK(xlib, "libX11");
        FN(xlib,void *,XOpenDisplay,(const char *)); FN(xlib,int,XDefaultScreen,(void *));
        FN(xlib,void *,XDefaultVisual,(void *,int)); FN(xlib,unsigned long,XVisualIDFromVisual,(void *));
        FN(xlib,unsigned long,XRootWindow,(void *,int));
        FN(xlib,unsigned long,XCreateSimpleWindow,(void *,unsigned long,int,int,unsigned,unsigned,unsigned,unsigned long,unsigned long));
        FN(xlib,int,XStoreName,(void *,unsigned long,const char *));
        FN(xlib,int,XMapWindow,(void *,unsigned long)); FN(xlib,int,XSync,(void *,int));
        XDestroyWindow = sym(xlib,"XDestroyWindow"); XCloseDisplay = sym(xlib,"XCloseDisplay");
        native = XOpenDisplay(NULL); CHECK(native,"XOpenDisplay");
        int screen = XDefaultScreen(native);
        visual_id = XVisualIDFromVisual(XDefaultVisual(native,screen));
        window = XCreateSimpleWindow(native,XRootWindow(native,screen),20,20,320,240,0,0,0);
        CHECK(window,"XCreateSimpleWindow"); XStoreName(native,window,"T736 IMG EGL GLES test");
        XMapWindow(native,window); XSync(native,0); display = eglGetDisplay(native);
    } else {
        struct stat st;
        fd = open(argv[2],O_RDWR|O_CLOEXEC); CHECK(fd >= 0,"open render node");
        CHECK(fstat(fd,&st)==0 && S_ISCHR(st.st_mode),"render character device");
        gbm = open_lib("libgbm.so.1");
        FN(gbm,void *,gbm_create_device,(int));
        FN(gbm,void *,gbm_surface_create,(void *,uint32_t,uint32_t,uint32_t,uint32_t));
        gbm_device_destroy=sym(gbm,"gbm_device_destroy"); gbm_surface_destroy=sym(gbm,"gbm_surface_destroy");
        gbm_surface_lock_front_buffer=sym(gbm,"gbm_surface_lock_front_buffer");
        gbm_surface_release_buffer=sym(gbm,"gbm_surface_release_buffer"); gbm_bo_get_fd=sym(gbm,"gbm_bo_get_fd");
        device=gbm_create_device(fd); CHECK(device,"gbm_create_device");
        visual_id=0x34325258U; /* DRM_FORMAT_XRGB8888 */
        gbm_surface=gbm_surface_create(device,320,240,(uint32_t)visual_id,1U<<2);
        CHECK(gbm_surface,"gbm_surface_create");
        FN(egl,void *,eglGetProcAddress,(const char *));
        EGLDisplay (*get_platform)(unsigned,void *,const EGLint *)=eglGetProcAddress("eglGetPlatformDisplayEXT");
        CHECK(get_platform,"eglGetPlatformDisplayEXT"); display=get_platform(0x31d7,device,NULL);
    }
    CHECK(display && eglInitialize(display,&major,&minor),"eglInitialize");
    printf("EGL %d.%d vendor=%s\n",major,minor,eglQueryString(display,0x3053));
    CHECK(eglBindAPI(0x30a0),"eglBindAPI ES");
    const EGLint attrs[]={0x3033,4,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3038};
    CHECK(eglChooseConfig(display,attrs,configs,128,&count),"eglChooseConfig");
    for(int i=0;i<count;i++){ EGLint id=0; eglGetConfigAttrib(display,configs[i],0x302e,&id); if((unsigned long)(unsigned)id==visual_id){config=configs[i];break;} }
    CHECK(config,"matching native visual config");
    const EGLint ctx_attrs[]={0x3098,2,0x3038};
    context=eglCreateContext(display,config,NULL,ctx_attrs); CHECK(context,"eglCreateContext ES2");
    surface=eglCreateWindowSurface(display,config,is_x11?(void *)(uintptr_t)window:gbm_surface,NULL);
    CHECK(surface,"eglCreateWindowSurface"); CHECK(eglMakeCurrent(display,surface,surface,context),"eglMakeCurrent");
    const char *renderer=(const char *)glGetString(0x1f01), *vendor=(const char *)glGetString(0x1f00);
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n",vendor?vendor:"null",renderer?renderer:"null",glGetString(0x1f02));
    CHECK(renderer && vendor,"GL strings");
    CHECK(!strstr(renderer,"llvmpipe") && !strstr(renderer,"softpipe") && !strstr(renderer,"Software"),"reject software renderer");
    CHECK(strstr(vendor,"Imagination") || strstr(renderer,"BXM") || strstr(renderer,"PowerVR"),"IMG hardware renderer identity");
    const unsigned char colors[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}};
    glEnable(0x0c11);
    for(int frame=0;frame<60;frame++){
        for(int q=0;q<4;q++){glScissor((q%2)*160,(q/2)*120,160,120);glClearColor(colors[q][0]/255.f,colors[q][1]/255.f,colors[q][2]/255.f,1.f);glClear(0x4000);}
        glFinish();
        for(int q=0;q<4;q++){unsigned char pixel[4]={0};glReadPixels((q%2)*160+80,(q/2)*120+60,1,1,0x1908,0x1401,pixel);for(int c=0;c<3;c++) CHECK(abs((int)pixel[c]-colors[q][c])<=2,"GPU pixel readback");}
        CHECK(glGetError()==0,"GLES error"); CHECK(eglSwapBuffers(display,surface),"eglSwapBuffers");
        if(is_gbm){void *bo=gbm_surface_lock_front_buffer(gbm_surface);CHECK(bo,"GBM front buffer");int dmabuf=gbm_bo_get_fd(bo);gbm_surface_release_buffer(gbm_surface,bo);CHECK(dmabuf>=0,"DMA-BUF export");close(dmabuf);}
        struct timespec delay={0,16000000}; nanosleep(&delay,NULL);
    }
    puts("GPU_PROBE_PASS frames=60 quadrants=4 readback=verified (not a GNOME or physical-monitor test)");
cleanup:
    if(display){eglMakeCurrent(display,NULL,NULL,NULL);if(surface)eglDestroySurface(display,surface);if(context)eglDestroyContext(display,context);eglTerminate(display);}
    if(gbm_surface)gbm_surface_destroy(gbm_surface);
    if(device)gbm_device_destroy(device);
    if(fd>=0)close(fd);
    if(native && window)XDestroyWindow(native,window);
    if(native)XCloseDisplay(native);
    return failed?1:0;
}
