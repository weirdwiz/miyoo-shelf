// Recovery after a crashed SDL overlay: restore the live emulator's tty and page 0.
// Does not terminate, pause, or reboot the game. Pass its tty number (from /proc/PID/fd).
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    int target = argc == 2 ? atoi(argv[1]) : 0;
    if (target < 1 || target > 63) { fprintf(stderr, "usage: %s <emulator tty number>\n", argv[0]); return 2; }
    int fd = open("/dev/tty0", O_RDWR);
    struct vt_stat vt;
    if (fd < 0 || ioctl(fd, VT_GETSTATE, &vt)) { perror("VT_GETSTATE"); return 1; }
    if (vt.v_active != target) {
        if (ioctl(fd, VT_UNLOCKSWITCH) || ioctl(fd, VT_ACTIVATE, target)) { perror("VT_ACTIVATE"); return 1; }
        for (int i = 0; i < 20; i++) {
            usleep(100000);
            if (ioctl(fd, VT_GETSTATE, &vt)) return 1;
            if (vt.v_active == target) break;
        }
    }
    close(fd);
    if (vt.v_active != target) { fprintf(stderr, "console did not return to tty%d\n", target); return 1; }
    fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo var;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var)) { perror("framebuffer"); return 1; }
    var.yoffset = 0;
    if (ioctl(fd, FBIOPAN_DISPLAY, &var)) { perror("FBIOPAN_DISPLAY"); return 1; }
    close(fd);
    printf("restored emulator tty%d and framebuffer page 0\n", target);
    return 0;
}
