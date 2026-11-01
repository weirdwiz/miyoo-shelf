// On-device assertion: a settled overlay must retain the visible framebuffer.
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) return 2;
    size_t len = (size_t)fix.line_length * var.yres_virtual;
    volatile unsigned char *mem = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) return 2;
    uint32_t last = 0;
    int changes = 0;
    for (int sample = 0; sample < 200; sample++) {
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var)) return 2;
        uint32_t hash = 2166136261u;
        // Sparse full-screen sampling catches a game replacing the switcher, cheaply.
        for (int y = 80; y < (int)var.yres - 80; y += 19)
            for (int x = 8; x < var.xres; x += 23) {
                size_t p = (size_t)(var.yoffset + y) * fix.line_length + x * 4;
                for (int b = 0; b < 4; b++) hash = (hash ^ mem[p + b]) * 16777619u;
            }
        if (sample && hash != last) changes++;
        last = hash;
        usleep(10000);
    }
    munmap((void *)mem, len);
    close(fd);
    printf("settled overlay: %d visible framebuffer changes in 2 seconds (%s)\n", changes, changes ? "FAIL: flicker/overwrite" : "PASS");
    return changes ? 1 : 0;
}
