#!/bin/sh
# Builds fixture/SDCARD: a small Onion-shaped SD card for the simulator.
# ROMs are empty placeholder files. Box art comes from libretro-thumbnails, scaled to
# 250px wide like Onion's scraper does. A few titles deliberately have no art, to test
# the placeholder tiles.
set -eu
cd "$(dirname "$0")/.."
SD=fixture/SDCARD
mkdir -p "$SD/Emu" "$SD/Roms"

emu() { # dir label extlist
    mkdir -p "$SD/Emu/$1" "$SD/Roms/$1/Imgs"
    cat > "$SD/Emu/$1/config.json" <<EOF
{
"label":"$2",
"launch":"launch.sh",
"rompath":"../../Roms/$1",
"imgpath":"../../Roms/$1/Imgs",
"extlist":"$3"
}
EOF
    printf '#!/bin/sh\necho "would run: $0 $*"\n' > "$SD/Emu/$1/launch.sh"
}

rom() { # dir libretro-repo ext "No-Intro name"
    : > "$SD/Roms/$1/$4.$3"
    img="$SD/Roms/$1/Imgs/$4.png"
    [ -f "$img" ] && return 0
    url="https://raw.githubusercontent.com/libretro-thumbnails/$2/master/Named_Boxarts/$(python3 -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$4").png"
    if [ -n "${SHELF_ART_CACHE:-}" ] && [ -f "$SHELF_ART_CACHE/$4.png" ] && cp "$SHELF_ART_CACHE/$4.png" "$img" || curl -sfL --connect-timeout 5 --max-time 20 -o "$img" "$url"; then
        sips -Z 250 "$img" >/dev/null 2>&1 || true
        echo "art   $4"
    else
        rm -f "$img"
        echo "noart $4"
    fi
}

emu GBA GBA "gba|zip"
emu SFC SFC "sfc|smc|zip"
emu GB GB "gb|zip"
emu GBC GBC "gbc|zip"
emu FC FC "nes|zip"
emu PS PS "cue|chd|pbp"
emu MD MD "md|gen|bin|zip"

GBA=Nintendo_-_Game_Boy_Advance; SFC=Nintendo_-_Super_Nintendo_Entertainment_System
rom GBA $GBA gba "Pokemon - Emerald Version (USA, Europe)"
rom GBA $GBA gba "Metroid Fusion (USA)"
rom GBA $GBA gba "Legend of Zelda, The - The Minish Cap (USA)"
rom GBA $GBA gba "Advance Wars (USA)"
rom GBA $GBA gba "Golden Sun (USA, Europe)"
rom GBA $GBA gba "Castlevania - Aria of Sorrow (USA)"
rom GBA $GBA gba "Kirby & The Amazing Mirror (USA)"
rom GBA $GBA gba "Mario Kart - Super Circuit (USA)"
rom GBA $GBA gba "Homebrew Puzzle Thing (World)"
rom SFC $SFC sfc "Super Mario World (USA)"
rom SFC $SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
rom SFC $SFC sfc "Legend of Zelda, The - A Link to the Past (USA)"
rom SFC $SFC sfc "Chrono Trigger (USA)"
rom SFC $SFC sfc "Donkey Kong Country (USA) (Rev 2)"
rom GB Nintendo_-_Game_Boy gb "Tetris (World) (Rev 1)"
rom GB Nintendo_-_Game_Boy gb "Kirby's Dream Land (USA, Europe)"
rom GBC Nintendo_-_Game_Boy_Color gbc "Pokemon - Crystal Version (USA, Europe) (Rev 1)"
rom FC Nintendo_-_Nintendo_Entertainment_System nes "Super Mario Bros. 3 (USA)"
rom PS Sony_-_PlayStation cue "Crash Bandicoot (USA)"
rom PS Sony_-_PlayStation cue "Castlevania - Symphony of the Night (USA)"
rom MD Sega_-_Mega_Drive_-_Genesis md "Sonic The Hedgehog (USA, Europe)"

entry() { # dir ext name
    printf '{"label":"%s","launch":"/mnt/SDCARD/Emu/%s/launch.sh","type":5,"rompath":"/mnt/SDCARD/Emu/%s/../../Roms/%s/%s.%s"}\n' "$3" "$1" "$1" "$1" "$3" "$2"
}
{
    entry GBA gba "Pokemon - Emerald Version (USA, Europe)"
    entry SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
    entry PS cue "Crash Bandicoot (USA)"
    entry GBA gba "Legend of Zelda, The - The Minish Cap (USA)"
    entry GB gb "Tetris (World) (Rev 1)"
} > "$SD/Roms/recentlist.json"
{
    entry GBA gba "Pokemon - Emerald Version (USA, Europe)"
    entry SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
    entry GB gb "Tetris (World) (Rev 1)"
} > "$SD/Roms/favourite.json"
# Onion's play activity DB: one rom row per game, one play_activity row per session.
PA="$SD/Saves/CurrentProfile/play_activity"
if command -v sqlite3 >/dev/null; then
    mkdir -p "$PA"
    rm -f "$PA/play_activity_db.sqlite"
    sqlite3 "$PA/play_activity_db.sqlite" <<'EOF'
CREATE TABLE rom(id INTEGER PRIMARY KEY, type TEXT, name TEXT, file_path TEXT, image_path TEXT, created_at INTEGER DEFAULT (strftime('%s', 'now')), updated_at INTEGER);
CREATE TABLE play_activity(rom_id INTEGER, play_time INTEGER, created_at INTEGER DEFAULT (strftime('%s', 'now')), updated_at INTEGER);
INSERT INTO rom(id, name, file_path) VALUES
 (1, 'Castlevania - Aria of Sorrow (USA)', 'GBA/Castlevania - Aria of Sorrow (USA).gba'),
 (2, 'Pokemon - Emerald Version (USA, Europe)', 'GBA/Pokemon - Emerald Version (USA, Europe).gba'),
 (3, 'Super Metroid (Japan, USA) (En,Ja)', 'SFC/Super Metroid (Japan, USA) (En,Ja).sfc'),
 (4, 'Advance Wars (USA)', 'GBA/Advance Wars (USA).gba');
INSERT INTO play_activity(rom_id, play_time, created_at) VALUES
 (1, 14400, strftime('%s', 'now') - 9 * 86400), (1, 9720, strftime('%s', 'now') - 86400),
 (2, 3000, strftime('%s', 'now') - 3600), (3, 41000, strftime('%s', 'now') - 3 * 86400),
 (4, 40, strftime('%s', 'now') - 40 * 86400);
EOF
fi
# Switcher screenshots, named as Onion names them (romScreens/<hash of rompath>.png): in-game
# snaps from libretro-thumbnails at 640x480. Tetris has none, to show the box-art card.
RS="$SD/Saves/CurrentProfile/romScreens"
mkdir -p "$RS"
grep -v "Tetris" "$SD/Roms/recentlist.json" | while IFS= read -r line; do
    rompath=$(printf '%s' "$line" | sed 's/.*"rompath":"\([^"]*\)".*/\1/')
    stem=$(basename "$rompath"); stem=${stem%.*}; sys=$(basename "$(dirname "$rompath")")
    case $sys in
    GBA) repo=$GBA ;; SFC) repo=$SFC ;; PS) repo=Sony_-_PlayStation ;; *) continue ;;
    esac
    name=$(python3 -I - "$rompath" <<'PY'
import sys
# Onion's FNV1A_Pippip_Yurii (src/common/utils/hash.h), little-endian.
s = sys.argv[1].encode(); n = len(s); M = (1 << 64) - 1
h, P = 14695981039346656037, 591798841
word = lambda o: int.from_bytes(s[o:o + 8], "little")
if n > 8:
    cycles = ((n - 1) >> 4) + 1; head = n - (cycles << 3)
    for c in range(cycles):
        h = ((h ^ word(c * 8)) * P) & M
        h = ((h ^ word(c * 8 + head)) * P) & M
else:
    h = ((h ^ word(0)) * P) & M
h32 = (h ^ (h >> 32)) & 0xffffffff
print(h32 ^ (h32 >> 16))
PY
)
    [ -f "$RS/$name.png" ] && continue
    url="https://raw.githubusercontent.com/libretro-thumbnails/$repo/master/Named_Snaps/$(python3 -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$stem").png"
    if curl -sfL --connect-timeout 5 --max-time 20 -o "$RS/$name.png" "$url"; then
        sips -z 480 640 "$RS/$name.png" >/dev/null 2>&1
        echo "screen $stem"
    else
        rm -f "$RS/$name.png"
        echo "noscreen $stem"
    fi
done
echo "fixture ready: $SD"
