# Test VM

Shelf is tested only in a full-system QEMU ARMv7 VM standing in for the Miyoo Mini: Ubuntu
22.04's armhf cloud image (glibc 2.35, so binaries from the device toolchain's glibc 2.28
run unchanged), 2 Cortex-A15 cores, 1 GiB. Nothing here touches a real device.

```sh
make vm-image     # downloads the image (checksum-pinned) into build/vm, makes the disk + seed
make vm-start     # boots headless; about 2 min the first time (cloud-init), ~1 min after
make vm-selftest  # device-toolchain binary runs, fb0 pans to its second page, keys arrive
make vm-stop      # powers off
```

| Script | |
|---|---|
| `start.sh` | boots in the background; ssh on 127.0.0.1:2222 (`VM_SSH_PORT`) |
| `ssh.sh 'cmd'` | runs a command as root |
| `key.sh a right 600 menu` | presses buttons (a number after a name is its hold time in ms) |
| `shot.sh out.png` | saves what the screen scans out now |
| `pixel.py out.png x,y` | prints pixel colours from a screenshot |
| `stop.sh` | powers off |

To play by hand, boot with a window: `VM_DISPLAY=cocoa sh tools/vm/start.sh` (`gtk` on Linux; stop a
headless VM first), then `make mm dist && sh tools/vm/run_shelf.sh`. With the window
focused the keyboard is the device's buttons: arrows = d-pad, Space = A, Left Ctrl = B,
Left Shift = X, Left Alt = Y, E / T = L1 / R1, Tab / Backspace = L2 / R2, Enter = START,
Right Ctrl = SELECT, Esc = MENU. Ctrl-Alt-G releases a grabbed mouse/keyboard.

Shares (9p): `build/vm/sdcard` (or `VM_SDCARD`) is the guest's `/mnt/SDCARD`; the repo is
`/mnt/repo`, read-only.

`VM_PANEL=752x560 make vm-start` boots with the panel of a Mini v4 or a later Mini Plus
(default 640x480), for checking Shelf's centred canvas. `shot.sh` reads the page on screen
from inside the guest whenever Shelf has flipped to its second page, which the host's
screendump can't see (`fbpage.py`).

What matches the device: `/dev/fb0` is 640x480, 32 bpp, two pages (`yres_virtual` 960) and
`FBIOPAN_DISPLAY` flips between them. Keys arrive as the Miyoo's codes on the virtio
keyboard (`/dev/input/event1`); POWER (`key.sh power`) arrives as KEY_POWER on `gpio-keys`
(`event0`), short press only. logind ignores it so Shelf can handle it.

Differences: no vblank wait (pans return at once, so the menus pace themselves by the
clock, as games always do);
writes to the framebuffer reach the screen after up to ~100 ms (virtio-gpu flushes
mapped pages lazily), so wait before a screenshot; a screendump only ever shows page 0
(the host scanout is one page tall), so `selftest.sh` checks the panned-to page by
reading `/dev/fb0` inside the guest; the virtio-gpu host resource is one
page tall, so a screendump cannot show the panned-to second page (selftest reads it inside
the guest instead); no MI_AO audio, battery, backlight or GPIO (Shelf falls back to its
null/file audio sink); the CPU is emulated (TCG), so timings say nothing about the Cortex-A7.

## Linux

Needs `qemu-system-arm` and `qemu-img`, podman (or docker), python3, and `xorrisofs`,
`genisoimage` or `mkisofs` for the cloud-init seed (macOS uses `hdiutil`). On an immutable
distro `brew install qemu` (Linuxbrew) is enough; its build has virtio-9p and gtk but no
sdl, so `VM_DISPLAY=gtk` is the window. The guest is always TCG, so a first boot takes about 4 min on an x86 desktop. The toolchain
containers mount the repo with `:z` so SELinux lets them read it. Screenshots are written
by `tools/img.py` (no sips, ImageMagick or ffmpeg needed); `record.py` still wants ffmpeg.
