/* Read the visible framebuffer through its normal fbdev API as a PPM image. */
#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) ||
        ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("fb info"); return 1; }
    if (v.bits_per_pixel != 32 || v.red.length != 8 || v.green.length != 8 ||
        v.blue.length != 8 || !v.xres || !v.yres || v.xres > 8192 || v.yres > 8192)
        return 2;
    uint32_t *row = malloc(v.xres * 4);
    unsigned char *rgb = malloc(v.xres * 3);
    if (!row || !rgb) return 3;
    printf("P6\n%u %u\n255\n", v.xres, v.yres);
    for (unsigned y = 0; y < v.yres; ++y) {
        off_t off = (off_t)(y + v.yoffset) * f.line_length + v.xoffset * 4;
        if (pread(fd, row, v.xres * 4, off) != (ssize_t)(v.xres * 4)) {
            perror("read framebuffer"); return 4;
        }
        for (unsigned x = 0; x < v.xres; ++x) {
            rgb[3*x] = row[x] >> v.red.offset;
            rgb[3*x+1] = row[x] >> v.green.offset;
            rgb[3*x+2] = row[x] >> v.blue.offset;
        }
        if (fwrite(rgb, 3, v.xres, stdout) != v.xres) return 5;
    }
    free(row); free(rgb); close(fd);
    return fflush(stdout) ? 6 : 0;
}
