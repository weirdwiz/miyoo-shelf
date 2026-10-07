# Shelf

A DSi/3DS-style home screen for the Miyoo Mini Plus that runs on top of
[Onion OS](https://github.com/OnionUI/Onion). Power on, see your games as square icons,
press A to play.

- **Home row:** Recent games first, then Library folders (All, Favorites, one per console)
  at the end of the same row. Springy scrolling with a bit of overshoot, like the DS.
- **Folders:** a 3-row grid that scrolls sideways. Y changes the sort (A–Z, Recent,
  Favorites).
- **Icons:** "blur fill" squares made from Onion's scraped art, cached on the SD card.

| Home | Library | Folder |
|---|---|---|
| ![](docs/screenshots/1-home.png) | ![](docs/screenshots/2-library.png) | ![](docs/screenshots/3-folder.png) |

## How it fits into Onion

Shelf is a separate app, not a fork. It only uses Onion's file formats:

| Onion piece | How Shelf uses it |
|---|---|
| `Emu/<SYS>/config.json` | List of consoles, ROM folder, file extensions, launch script |
| `Roms/<SYS>/Imgs/<rom>.png` | Box art from Onion's scraper. Shelf never scrapes |
| `Roms/recentlist.json`, `Roms/favourite.json` | Recent games and favorites, read and written in MainUI's format |
| `/tmp/cmd_to_run.sh` | Shelf writes the game command here and exits. Onion's runtime runs the game, then starts Shelf again |

Installing on the device (not built yet): Onion's runtime bind-mounts
`.tmp_update/bin/MainUI-<device>-clean|expert` as the menu program on every loop. The
installer swaps those for a small script that starts Shelf, or stock MainUI when Shelf is
turned off.

## Develop on the Mac

Needs SDL2 (`brew install sdl2`).

```sh
make run      # builds the simulator and a sample SD card in fixture/, then opens a 640x480 window
make shots    # headless screenshots into docs/screenshots
```

Keys: arrows · `Z`/`Enter` = A · `X`/`Backspace` = B · `Y` = Y · `Q`/`W` = L/R · `Esc` quits.
`SHELF_SCALE=3 make run` makes the window bigger.

Point the simulator at a copy of your real SD card:

```sh
SHELF_ROOT=/Volumes/SDCARD SHELF_FONTS=$PWD/assets/fonts SHELF_CMD=/tmp/cmd.sh build/shelf-sim
```

Everything is drawn in software into a 640x480 buffer. `src/platform_*.c` is the only
code that differs per target, so the simulator shows exactly what the device will.

## Layout

```
src/main.c          loop, headless screenshot mode
src/ui.c            home row, folder grid, springs, launch animation
src/library.c       Onion SD card reader, launch + recents writer
src/icons.c         blur-fill icon builder and cache (App/Shelf/cache)
src/gfx.c, text.c   software canvas, stb_truetype text
src/platform_sdl2.c simulator window and keyboard
tools/make_fixture.sh  sample SD card with Libretro box art
```

## Credits

- [stb_image, stb_truetype](https://github.com/nothings/stb) (public domain / MIT)
- [cJSON](https://github.com/DaveGamble/cJSON) (MIT, `third_party/cJSON.LICENSE`)
- M PLUS Rounded 1c, © 2016 The Rounded M+ Project Authors, SIL Open Font License 1.1
- Onion OS team, and Allium for the simulator-first idea
