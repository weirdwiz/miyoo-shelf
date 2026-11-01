"""Records the VM's screen to a video while a command runs (usually key presses).

    python3 -I tools/vm/record.py out.mp4 [--fps 10] -- sh tools/vm/key.sh right right a

Grabs screendumps over its own QEMU monitor (tools/vm/start.sh opens two), then encodes
them with ffmpeg at the measured frame rate, turned 180 degrees like tools/vm/shot.sh
(the panel is upside down).
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

args = sys.argv[1:]
out = args.pop(0)
fps = 10
if args and args[0] == "--fps":
    args.pop(0)
    fps = int(args.pop(0))
if args and args[0] == "--":
    args.pop(0)
# Its own monitor: key.sh keeps the other.
mon = os.path.join("build", "vm", "monitor-rec.sock")
tmp = tempfile.mkdtemp(prefix="shelf-rec-", dir=os.path.abspath("build/vm"))


def until_prompt(s):
    buf = b""
    while not buf.endswith(b"(qemu) "):
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("monitor closed")
        buf += chunk


conn = socket.socket(socket.AF_UNIX)
conn.connect(mon)
conn.settimeout(5)
until_prompt(conn)


def screendump(path):
    conn.sendall(f"screendump {path}\n".encode())
    until_prompt(conn)


stop = threading.Event()
frames = []


def grab():
    n = 0
    t0 = time.time()
    while not stop.is_set():
        path = os.path.join(tmp, f"f{n:05d}.ppm")
        screendump(path)
        frames.append((time.time() - t0, path))
        n += 1
        delay = n / fps - (time.time() - t0)
        if delay > 0:
            time.sleep(delay)


t = threading.Thread(target=grab)
t.start()
time.sleep(0.5)
rc = subprocess.call(args) if args else 0
time.sleep(1.0)
stop.set()
t.join()

# Constant-rate video: for each output tick use the latest frame captured by then.
dur = frames[-1][0]
seq = os.path.join(tmp, "seq")
os.mkdir(seq)
j = 0
for k in range(int(dur * fps) + 1):
    while j + 1 < len(frames) and frames[j + 1][0] <= k / fps:
        j += 1
    os.link(frames[j][1], os.path.join(seq, f"s{k:05d}.ppm"))
subprocess.check_call(
    [
        "ffmpeg", "-loglevel", "error", "-y", "-framerate", str(fps),
        "-i", os.path.join(seq, "s%05d.ppm"),
        "-vf", "hflip,vflip,scale=640:480:flags=neighbor",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", out,
    ]
)
shutil.rmtree(tmp)
print(f"recorded {len(frames)} grabs over {dur:.1f} s -> {out}")
sys.exit(rc)
