// VM self-test: framebuffer geometry, page flipping and evdev keys, built for the device.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fb = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &f)) {
        perror("fb0");
        return 1;
    }
    printf("fb %ux%u bpp %u virtual %ux%u line %u smem %u\n", v.xres, v.yres, v.bits_per_pixel,
           v.xres_virtual, v.yres_virtual, f.line_length, f.smem_len);
    if (v.yres_virtual < 2 * v.yres) { printf("FAIL: one page only\n"); return 1; }
    uint8_t *m = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    for (unsigned y = 0; y < 2 * v.yres; y++) {
        uint32_t *row = (uint32_t *)(m + (size_t)y * f.line_length);
        for (unsigned x = 0; x < v.xres; x++) row[x] = y < v.yres ? 0xff0000 : 0x0000ff;
    }
    v.yoffset = v.yres;
    if (ioctl(fb, FBIOPAN_DISPLAY, &v)) { perror("pan"); return 1; }
    printf("panned to page 1\n");
    fflush(stdout);
    if (argc < 2) return 0;
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { perror(argv[1]); return 1; }
    struct input_event e;
    int keys = argc > 2 ? atoi(argv[2]) : 1;
    while (keys > 0 && read(in, &e, sizeof e) == sizeof e)
        if (e.type == EV_KEY && e.value == 1) { printf("key %d\n", e.code); fflush(stdout); keys--; }
    return 0;
}
