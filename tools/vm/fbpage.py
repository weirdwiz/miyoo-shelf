"""Runs inside the guest: prints the fb0 page on screen as a PPM, in framebuffer orientation.

The host's screendump only covers fb0's first page, so tools/vm/shot.sh streams this over
ssh when Shelf has flipped to its second one.
"""
import sys

fb = "/sys/class/graphics/fb0/"
pan_y = int(open(fb + "pan").read().split(",")[1])
stride = int(open(fb + "stride").read())
w, vh = (int(v) for v in open(fb + "virtual_size").read().split(","))
h = vh // 2  # two pages
with open("/dev/fb0", "rb") as f:
    f.seek(pan_y * stride)
    d = f.read(h * stride)
rgb = bytearray(w * h * 3)
for y in range(h):
    row = d[y * stride : y * stride + w * 4]  # XRGB8888, little endian: B G R X
    o = y * w * 3
    rgb[o + 0 : o + w * 3 : 3] = row[2::4]
    rgb[o + 1 : o + w * 3 : 3] = row[1::4]
    rgb[o + 2 : o + w * 3 : 3] = row[0::4]
sys.stdout.buffer.write(b"P6\n%d %d\n255\n" % (w, h) + rgb)
