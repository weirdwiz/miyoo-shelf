#!/usr/bin/env python3
"""Prepare a pinned Onion keymon with only Shelf's overlay handoff backported."""
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / "build/onion-4.3.1-1.tar.gz"
SOURCE = ROOT / "build/Onion-4.3.1-1"
SHA256 = "5f80c6676532a199de37e932e87c6a41c518e072f31d326b125424e2bbf50517"
URL = "https://codeload.github.com/OnionUI/Onion/tar.gz/refs/tags/v4.3.1-1"
ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
if not ARCHIVE.exists():
    urllib.request.urlretrieve(URL, ARCHIVE)
if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != SHA256:
    raise SystemExit(f"Checksum mismatch: {ARCHIVE}")
if not SOURCE.exists():
    with tarfile.open(ARCHIVE) as archive:
        archive.extractall(ARCHIVE.parent, filter="data")
header = SOURCE / "src/keymon/menuButtonAction.h"
original = """void action_RA_gameSwitcher(void)
{
    screenshot_system();
    set_gameSwitcher();
    terminate_retroarch();
}"""
replacement = '#include "shelf_overlay.h"'
text = header.read_text()
if original in text:
    header.write_text(text.replace(original, replacement, 1))
elif replacement not in text:
    raise SystemExit("Unexpected keymon source; refusing to patch")
shutil.copyfile(ROOT / "assets/compat/keymon-overlay.h", header.parent / "shelf_overlay.h")
print(f"Prepared {SOURCE}/src/keymon")
