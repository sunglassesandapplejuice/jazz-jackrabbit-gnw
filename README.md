# Jazz Jackrabbit for Game & Watch Retro-Go SD

<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/95700eb6-96dc-4474-b2d4-4574dfe4e283" />



Epic MegaGames' **Jazz Jackrabbit** (1994), PC DOS version, running on the
Nintendo Game & Watch with the
[Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd) firmware,
as a **GWHB homebrew**: a single `.bin` dropped in `/homebrews/`, no firmware
rebuild needed.

The game engine is **OpenJazz** (AJ Thomson, Carsten Teibes and
contributors, GPLv2,
[commercial-game-sources/openjazz](https://github.com/commercial-game-sources/openjazz)),
running on a small SDL 1.2 written for the console. The project is built from
[retro-go-sd-templates](https://github.com/sylverb/retro-go-sd-templates): it
talks to the firmware only through the firmware ABI.

**The game data is not included.** You need the files of Jazz Jackrabbit 1
for DOS: the full game (CD, GOG "Jazz Jackrabbit Collection"), the shareware
episode or Holiday Hare (`BLOCKS.000`, `LEVEL0.000`, `SPRITES.000`,
`PANEL.000`, `*.PSM`...).

## Installation

Requires Retro-Go SD **v2.0.0-rc1 or later** (firmware ABI v2).

1. Build the game data pack (Python 3 only, nothing else to install):
   ```
   python3 tools/jj_pack.py Jazz.jj /path/to/JAZZ
   ```
   On Windows: `py tools/jj_pack.py Jazz.jj C:\Games\Jazz`
2. Copy both files to the SD card, side by side, with the same name:
   ```
   /homebrews/Jazz.bin
   /homebrews/Jazz.jj
   ```
3. Start it from the **Homebrew** tab.

The first start copies the pack to the external flash (a few minutes); the
game then uses its data in place from there.

### What jj_pack.py does

The console has about 600 KB of RAM for the game; OpenJazz decodes about
1 MB of pictures for a level and plays its music with a module player that
loads whole modules in RAM. So `jj_pack.py` runs the game itself on your
computer (`jj_render`, shipped for Windows, macOS and Linux in `jj_render/`
next to the script) to:

- **pre-decode every picture** of the game (tiles, sprites, fonts, menus,
  scenes) into `SURFACES.BAK`: the console uses them in place from its flash;
- **render the music** (the `*.PSM` modules) to samples with their exact loop
  points: the console plays them back.

| Option | Pack size (full game) | Music |
|---|---|---|
| *(default)* | ~32 MB | IMA ADPCM, 11 kHz |
| `--music-codec pcm` | ~73 MB | 16 bit PCM, 11 kHz (no hiss) |
| `--no-music` | ~18 MB | none |

The pack must fit in the external flash of the console; if it does not, the
console says "Pack too big for the flash".

## Controls

| Game & Watch | Jazz Jackrabbit |
|---|---|
| D-pad | Move, look up (up), crouch (down); menus |
| A | Jump / swim up; validate in the menus |
| B | Fire; "no" in the menus |
| TIME (SELECT on Zelda units) | Change weapon |
| GAME (START on Zelda units) | Game menu / back (Esc) |
| PAUSE/SET | Retro-Go menu |

The Retro-Go menu adds savestates and, in *Options*: music, sound effects and
"Swap A/B" (A = fire, B = jump).

**Savestates** record the level, its last checkpoint, the difficulty and the
player (score, lives, ammo, fire rate, birds). Loading one restarts that
level from that checkpoint, like after losing a life (OpenJazz cannot save a
level in progress). They only work in the levels (not in the menus, bonus
levels or scenes).

The picture is 320x240, the whole screen: OpenJazz shows a bit more of the
level than the original 320x200.

## Building

Needs `arm-none-eabi-gcc` (hard-float, v10+), GNU Make, Python 3 + Pillow:

```
make            # → Jazz.bin
make docker     # same, inside the Retro-Go SD builder image
```

`jj_render` (the harness, below) is cross-built with zig:

```
pip install ziglang
tools/build_renderers.sh    # → tools/bin/jj_render-<os>-<arch>
```

### Testing without hardware

`harness/` builds the same game and port code for Linux, headless, with a
virtual clock, scripted buttons, PPM screenshots and WAV output:

```
make -C harness
harness/build/jj_host Jazz.jj -t 30000 -e 1000 -o /tmp/out -w /tmp/out/a.wav
harness/build/jj_host Jazz.jj -l 0:0 -s "10000:A:1,10200:A:0,14000:RIGHT:1" -S 20000 -o /tmp/out
```

`-l world:level` starts at a level, `-q file -Q ms` writes a savestate,
`-j file -J ms` loads one, `-L file` restores one at start, `-B file` /
`-R dir` make the pictures / music of the pack, `make -C harness LIVE=1
BUILD_DIR=build_live` traces the heap (`tools/live_report.py`). The pack is
mapped read-only, like the flash.

## Layout

| Path | Role |
|------|------|
| `src/oj/` | OpenJazz (commercial-game-sources/openjazz @ a1626f4), JJ1 only. Game & Watch changes are under `#ifdef GNW`. |
| `src/ext/` | xmp (music, PC only), miniz, stb_rect_pack. |
| `src/port/sdl/`, `jj_sdl.cpp` | The small SDL 1.2: 8 bit surfaces, blits, palettes, keys, audio. |
| `src/port/jj_bake.c` | Pre-decoded pictures (`SURFACES.BAK`). |
| `src/port/jj_sound.cpp`, `jj_music.c` | Sound effects in place, pre-rendered music. |
| `src/port/jj_state.cpp` | Savestates. |
| `src/port/jj_file.c`, `jj_pack.c` | Files of the game: in the pack (flash) or on the SD card. |
| `src/port/jj_main.c` | Retro-Go side: entry point, display, audio, buttons, pause menu. |
| `src/port/jj_render.cpp` | Music rendering (PC only). |
| `tools/jj_pack.py` | Builds the `.jj` pack. |
| `harness/` | Headless test harness, also `jj_render`. |
| `sdk/` | Retro-Go SD core / homebrew SDK (from retro-go-sd-templates). |

See `CLAUDE.md` for the internals.

## License

GPLv2 (see `LICENSE`), like OpenJazz. xmp, miniz and stb_rect_pack are MIT
(`src/ext/*/LICENSE`); the SDK under `sdk/` is MIT (`sdk/LICENSE.MIT`).
Jazz Jackrabbit is a game by Epic MegaGames (Arjan Brussee, Cliff
Bleszinski).
