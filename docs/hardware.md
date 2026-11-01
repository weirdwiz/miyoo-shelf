# Miyoo Mini / Mini Plus hardware notes

What Shelf touches directly now that it runs without Onion. These facts come from Onion
4.3.1-1's sources (`src/common/system/*.h`, `src/batmon`, `src/keymon`, `runtime.sh`) and
the SigmaStar SDK headers in the device toolchain (`mi_ao.h`, `mi_sys.h`). Lines marked
*unverified* have not been checked on hardware by Shelf.

## Boot

The stock firmware's `/customer/main` runs `/mnt/SDCARD/.tmp_update/updater` when it
exists, instead of the stock MainUI. Shelf's updater sets
`LD_LIBRARY_PATH=/lib:/config/lib:/customer/lib` (the `libmi_*.so` vendor libraries live
there), stops the stock `audioserver`/`audioserver.mod` if running (they hold
`/dev/mi_ao`), and runs Shelf. Taking `.tmp_update` off the card restores stock.

Model: the Mini Plus has an AXP223 PMIC on `/dev/i2c-1`, address `0x34`; a successful
read of its register 0 means Mini Plus (Onion: `axp 0`). The Plus also has
`/customer/app/axp_test`.

## Display

`/dev/fb0`, 640x480, 32 bpp XRGB, virtual height of at least two pages; pan with
`FBIOPAN_DISPLAY` (it waits for vblank). The panel is mounted upside down, so frames are
rotated 180 degrees before scan-out.

Later units (Mini v4 from late 2023, later Mini Plus) have a 752x560 panel timing with a
750x559 visible area. Onion reads it from `Current TimingWidth=` in
`/proc/mi_modules/fb/mi_fb0` and resizes fb0 with `fbset`; MinUI looks for `752x560p` in
`/sys/class/graphics/fb0/modes`. Shelf takes fb0 as it finds it: at 640x480 the canvas
fills it, and on anything larger the 640x480 canvas is centred with a black border.
*Unverified*: which size fb0 has at boot on those units, and whether the 180-degree
rotation holds for them.

## Hardware probe

For a first boot on a device: create an empty file `.shelf/probe` on the card and boot.
The boot script removes it (so the probe runs once), writes what the system reports to
`.shelf/data/probe.txt` (firmware version, CPU and its frequencies, fb0 and its modes,
input devices, battery tools, mounts), then Shelf adds what it sees and shows a test pattern
for about 17 seconds: fb0's geometry and timings, how fast frames go out (the panel's
refresh rate when pans wait for vblank), the battery, and every raw input event. Press
each button once while the pattern is up. Red should be top left, green top right, blue
bottom left and yellow bottom right; a photo of the counting squares in the middle times
the frame rate. Shelf then starts as usual.

Backlight: PWM. `echo 0 > /sys/class/pwm/pwmchip0/export`, then in
`/sys/class/pwm/pwmchip0/pwm0/`: `period` 800, `duty_cycle` 0-100, `enable` 1.
Brightness level 0-10 maps to duty `round(3 * exp(0.350656 * level))` (3..100).

Panel power: GPIO 4 (`/sys/class/gpio/export` "4", `gpio4/direction` "out",
`gpio4/value` 0 off / 1 on). After turning it on, re-enable the PWM (write `enable` 0
then 1).

## Input

`/dev/input/event0`, `EV_KEY`. Codes: UP 103, DOWN 108, LEFT 105, RIGHT 106,
A 57 (SPACE), B 29 (LEFTCTRL), X 42 (LEFTSHIFT), Y 56 (LEFTALT), L1 18 (E), R1 20 (T),
L2 15 (TAB), R2 14 (BACKSPACE), SELECT 97 (RIGHTCTRL), START 28 (ENTER), MENU 1 (ESC),
POWER 116, VOLUME UP 115 / DOWN 114 (Mini Plus only). Values: 1 press, 0 release,
2 autorepeat.

## Audio

SigmaStar MI_AO through `libmi_sys.so` and `libmi_ao.so`: `MI_SYS_Init`,
`MI_AO_SetPubAttr(0, &attr)`, `MI_AO_Enable(0)`, `MI_AO_EnableChn(0, 0)`, then
`MI_AO_SendFrame(0, 0, &frame, timeout_ms)` with interleaved s16 stereo. Shelf loads these
with `dlopen` so the same binary also runs where they are absent (the VM).

Volume in dB, -60..+30: `MI_AO_SetVolume(0, db)`, or the `/dev/mi_ao` ioctls
`MI_AO_SETVOLUME 0x4008690b`, `MI_AO_GETVOLUME 0xc008690c`, `MI_AO_SETMUTE 0x4008690d` with
`uint64_t arg[2] = {sizeof(int[2]), (uintptr_t)int[2]{0, value}}`. Level 0-20 maps to
`round(48 * log10(1 + level)) - 60` dB; level 0 also mutes.

## Battery

- Mini: SAR ADC `/dev/sar`: `ioctl(fd, _IO('a', 0))` once to init, then
  `ioctl(fd, _IO('a', 1), &(struct {int channel; int adc;}){0})`. Voltage is
  `(adc - 148) * 0.01`; percentage linearly interpolates Onion's measured Li-ion curve
  (4.20 V = 100%, 4.13 V = 95%, through 3.00 V = 0%). Readings from the last 90 s are
  averaged; while discharging the shown percentage only rises once the average has stayed
  above it for three readings, and plugging in or out starts afresh.
  Charging: GPIO 59 (export, direction "in"), value 1. *Unverified on hardware.*
- Mini Plus: AXP223 over i2c (`I2C_RDWR`): charging is register 0x00 bit 2; battery percent
  is register 0xB9 bits 0-6. The `/customer/app/axp_test` fallback marks charging only when
  its `charging` field is 3 (Onion's `battery_isCharging`; that is axp_test's own value, not
  the register bit). *Unverified on hardware.*

## Power

There's no kernel suspend (`/sys/power/state` offers none). Standby is a software job:
pause emulation, mute audio, backlight 0, panel off (GPIO 4), CPU governor `powersave`
(`/sys/devices/system/cpu/cpufreq/policy0/scaling_governor`), then wait for POWER.
Power off: `sync`, then `poweroff` (*unverified* on the Plus, where Onion uses its own
`shutdown` script).
