"""PPM to PNG/PPM with no dependencies (stands in for macOS's sips, so it runs anywhere).

    python3 -I tools/img.py [--rotate180] in.ppm out.png|out.ppm
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    # P6 <w> <h> <maxval> <single whitespace> <binary>; '#' comments may sit between fields.
    fields, i = [], 0
    while len(fields) < 4:
        while d[i : i + 1].isspace():
            i += 1
        if d[i : i + 1] == b"#":
            while d[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not d[j : j + 1].isspace():
            j += 1
        fields.append(d[i:j])
        i = j
    i += 1
    if fields[0] != b"P6" or fields[3] != b"255":
        sys.exit(f"img: {path}: not an 8-bit P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, d[i : i + w * h * 3]


def rotate180(w, h, rgb):
    # Reversing whole pixels (3-byte groups) turns the image half a turn.
    out = bytearray(len(rgb))
    for c in range(3):
        out[c::3] = rgb[c::3][::-1]
    return bytes(out)


def png(w, h, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    raw = b"".join(b"\0" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


args = sys.argv[1:]
rot = "--rotate180" in args
args = [a for a in args if a != "--rotate180"]
if len(args) != 2:
    sys.exit(__doc__)
w, h, rgb = read_ppm(args[0])
if rot:
    rgb = rotate180(w, h, rgb)
if args[1].endswith(".png"):
    data = png(w, h, rgb)
else:
    data = b"P6\n%d %d\n255\n" % (w, h) + rgb
with open(args[1], "wb") as f:
    f.write(data)
