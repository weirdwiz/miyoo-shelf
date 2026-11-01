# libretro cores for the Miyoo Mini

Shelf runs games through libretro cores. This directory builds the two it ships
and fetches freely redistributable ROMs to test them with.

```sh
make cores      # cross-compile -> build/cores/mgba_libretro.so, gambatte_libretro.so
make testroms   # download      -> build/testroms/*.gb,*.gba,*.gbc
```

`make cores` is a no-op when both `.so` files are newer than the scripts. Delete
`build/cores` for a from-scratch build (~2.5 minutes here, see Build time).

## What gets built

| core | console | output |
| --- | --- | --- |
| mGBA | Game Boy Advance | `build/cores/mgba_libretro.so` |
| gambatte | Game Boy / Game Boy Color | `build/cores/gambatte_libretro.so` |

Pinned commits (bump them in `build.sh` and rebuild):

| core | repository | commit |
| --- | --- | --- |
| mGBA | https://github.com/libretro/mgba | `7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6` |
| gambatte | https://github.com/libretro/gambatte-libretro | `77f2567d718b8973d02caa2daa8dce4953faf811` |

`build/cores/VERSIONS` records the same pins next to the binaries. A rebuild of
the same commit produces byte-identical `.so` files (checked by rebuilding from a
clean `build/cores` and comparing SHA-256).

## How it is built

Everything compiles inside `docker.io/aemiii91/miyoomini-toolchain:latest`
(gcc 8.3, glibc 2.28, `arm-linux-gnueabihf-`), so the host needs podman or
docker only. The image is amd64 and runs under emulation on Apple Silicon.

- `build.sh` checks out each pinned commit (shallow `git fetch` of that SHA) into
  `build/cores/src/` and starts the container.
- `build-in-container.sh` compiles, strips, installs the licenses, writes
  `VERSIONS`, and then verifies both cores. `build-in-container.sh verify` re-runs
  just the checks.
- Device flags for both cores: `-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
  -mfloat-abi=hard -O2`. Neither core has a separate NEON path to switch on:
  gambatte has no SIMD sources, and mGBA's NEON renderer only exists in its
  GLES/GL backends, which are off because Shelf draws with fbdev.
- **mGBA** has no `Makefile.libretro` any more; it is built with its own CMake
  (`-DBUILD_LIBRETRO=ON`, `-DSKIP_LIBRARY=ON`) as a cross build. OpenGL, GLES,
  zlib, PNG, SQLite, FFmpeg, ELF and Lua support are off; the core's CMake sets
  `DISABLE_THREADING` and `-O3` for the libretro target itself.
- **gambatte** uses its own `Makefile.libretro` with `platform=unix`,
  `HAVE_NETWORK=0` (drops the socket link-cable transport), and links libstdc++
  and libgcc statically (`-static-libstdc++ -static-libgcc`) so the device needs
  no C++ runtime.
- Cores are stripped with `arm-linux-gnueabihf-strip --strip-unneeded`.

## Verification (part of `make cores`)

| check | mGBA | gambatte |
| --- | --- | --- |
| `file` | ELF 32-bit ARM, EABI5, hard-float ABI, VFP registers | same |
| size, stripped | 1551472 bytes | 1697380 bytes |
| `NEEDED` | `libm.so.6`, `libpthread.so.0`, `libc.so.6` | `libm.so.6`, `libc.so.6`, `ld-linux-armhf.so.3` |
| max `GLIBC_` symbol version | 2.27 | 2.27 |
| exports | `retro_init`, `retro_run`, `retro_load_game`, `retro_serialize`, `retro_get_memory_data` | same |

No SDL, GL, X11, PNG or other library outside a minimal glibc 2.28 system is
needed, and no symbol requires glibc newer than 2.28, so both cores load on the
device as shipped.

## Licenses

`build/cores/<core>_libretro.LICENSE` holds the upstream license text next to each
binary; both must ship with the cores.

- **mGBA** is MPL-2.0 (`mgba_libretro.LICENSE`, from the mGBA `LICENSE`). The
  binary is a modified/compiled form of MPL-2.0 sources, so redistributing it
  requires the license text and access to the source at the pinned commit above.
- **gambatte** is GPL-2.0 (`gambatte_libretro.LICENSE`, from gambatte's
  `COPYING`). Shipping the `.so` means the whole offer to provide the
  corresponding source at the pinned commit above has to come with it.

## Build time

Measured on an Apple Silicon Mac with podman (amd64 emulation, 7 CPUs, `-j8`):

- from a clean `build/cores` (clone + configure + build both cores + verify):
  159 s (`make cores testroms`, ROMs included: 176 s)
- `make cores` with sources and objects present: ~7 s (CMake reconfigure, only
  the files that changed recompile)

## Test ROMs

`fetch-testroms.sh` (see `build/testroms/SOURCES` for SHA-256, license and URL of
everything it writes) downloads:

| file | what | license |
| --- | --- | --- |
| `cpu_instrs.gb`, `cpu_instrs-01-special.gb` | Blargg's Game Boy CPU tests | freely distributable test ROMs by Shay Green (no formal license file) |
| `dmg_sound.gb` | Blargg's DMG sound test | same |
| `gba-arm.gba`, `gba-thumb.gba`, `gba-memory.gba` | jsmolka's GBA tests | MIT |
| `ucity.gbc` | uCity 1.3, CGB city-building game (interactive test) | GPL-3.0 |
| `gematrix.gba` | Gematrix, GBA gem-matching game (interactive test) | GPL-3.0 |

Each file is checked against the pinned SHA-256 before use; the required blargg
and jsmolka ROMs fail the script on a mismatch, the two homebrew games are
skipped with a note if their release download is unavailable. `build/testroms/licenses/`
holds the license texts and the provenance readme. Upstream revisions are pinned
in the script: `retrio/gb-test-roms@c240dd7d`, `jsmolka/gba-tests@a7113b67`,
`AntonioND/ucity@v1.3` (d1880a2a), `Issung/Gematrix@ebb7852a`.
