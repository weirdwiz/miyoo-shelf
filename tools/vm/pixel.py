"""Prints the RGB of pixels in a screenshot.

    pixel.py shot.png x,y [x,y...]      (default: the centre pixel)
"""
import struct
import sys
import zlib


def load(path):
    """Decodes an 8-bit non-interlaced PNG (RGB, RGBA, gray) or a P6 PPM, no dependencies."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] == b"P6":
        w, h, _ = (int(v) for v in d.split(None, 4)[1:4])
        off = len(d) - w * h * 3
        return w, h, lambda x, y: tuple(d[off + (y * w + x) * 3 : off + (y * w + x) * 3 + 3])
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"pixel: {path}: not a PNG or PPM")
    i, idat, hdr = 8, b"", None
    while i < len(d):
        n, tag = struct.unpack_from(">I4s", d, i)
        body = d[i + 8 : i + 8 + n]
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        i += 12 + n
    w, h, depth, ctype, _, _, interlace = hdr
    ch = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or ch is None:
        sys.exit("pixel: only 8-bit non-interlaced gray/RGB/RGBA PNGs are supported")
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for x in range(stride):
            a = cur[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                cur[x] = (cur[x] + a) & 255
            elif f == 2:
                cur[x] = (cur[x] + b) & 255
            elif f == 3:
                cur[x] = (cur[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[x] = (cur[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(cur)
        prev = cur

    def px(x, y):
        i = x * ch
        r = rows[y]
        return (r[i],) * 3 if ch < 3 else (r[i], r[i + 1], r[i + 2])

    return w, h, px


w, h, px = load(sys.argv[1])
pts = [tuple(map(int, a.split(","))) for a in sys.argv[2:]] or [(w // 2, h // 2)]


def hexat(x, y):
    r, g, b = px(x, y)
    return f"{x},{y}={r:02x}{g:02x}{b:02x}"


print(f"{w}x{h}", " ".join(hexat(x, y) for x, y in pts))
