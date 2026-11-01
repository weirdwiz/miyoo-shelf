"""Prints the RGB of pixels in a screenshot.

    pixel.py shot.png x,y [x,y...]      (default: the centre pixel)
"""
import struct
import subprocess
import sys
import tempfile


def load(path):
    with tempfile.NamedTemporaryFile(suffix=".bmp") as t:
        subprocess.run(
            ["sips", "-s", "format", "bmp", path, "--out", t.name],
            check=True,
            capture_output=True,
        )
        with open(t.name, "rb") as f:
            d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    stride = (w * bpp + 3) & ~3

    def px(x, y):
        row = y if h < 0 else abs(h) - 1 - y
        i = off + row * stride + x * bpp
        return d[i + 2], d[i + 1], d[i]

    return w, abs(h), px


w, h, px = load(sys.argv[1])
pts = [tuple(map(int, a.split(","))) for a in sys.argv[2:]] or [(w // 2, h // 2)]


def hexat(x, y):
    r, g, b = px(x, y)
    return f"{x},{y}={r:02x}{g:02x}{b:02x}"


print(f"{w}x{h}", " ".join(hexat(x, y) for x, y in pts))
