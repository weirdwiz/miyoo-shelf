#!/usr/bin/env python3
"""Fetch explicitly mapped artwork into an Onion card (never touches ROMs)."""
import argparse
import json
from pathlib import Path
from urllib.parse import quote
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("manifest", type=Path)
parser.add_argument("root", type=Path, help="SD card root or local artwork staging folder")
args = parser.parse_args()
failed = False
for entry in json.loads(args.manifest.read_text()):
    target = args.root / entry["path"]
    if target.exists():
        print(f"keep  {entry['path']}")
        continue
    url = entry.get("url")
    if not url:
        url = (f"https://raw.githubusercontent.com/libretro-thumbnails/{entry['repo']}"
               f"/master/Named_Boxarts/{quote(entry['title'], safe='')}.png")
    try:
        request = Request(url, headers={"User-Agent": "Shelf-artwork/1.0"})
        with urlopen(request, timeout=30) as response:
            data = response.read()
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("source did not return a PNG")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        print(f"added {entry['path']}")
    except Exception as error:
        failed = True
        print(f"FAIL  {entry['path']}: {error}")
raise SystemExit(1 if failed else 0)
