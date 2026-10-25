# Diamondus Music Analysis & Bug Investigation

## 1. Diamondus Music Structure (`SONG0.PSM`)

* **Format**: Epic MegaGames MASI PSM module (4 tracker channels/voices, 7 patterns, 12 orders).
* **Order list**: `0 2 2 1 3 2 2 1 3 4 5 6` (duration ~104.8 seconds total at 22050 Hz).
* **Pattern 0 (Order 0, Rows 0–63, duration ~8.9 s)**:
  * **Channel 0**: Inactive (0 notes)
  * **Channel 1**: Active (lead melody riff with instruments 6, 7, 8)
  * **Channel 2**: Inactive (0 notes)
  * **Channel 3**: Active (bass/kick riff with instrument 2)
  * *Notice: In this introductory section, only **2 channels** play.*
* **Patterns 1–6 (Orders 1–11, from ~8.9 s onwards)**:
  * Channels 0, 1, 2, and 3 are all active (drums, bass, chord backing, and lead synth).
  * The full 4-channel arrangement plays.

---

## 2. Hypotheses for the Game & Watch Playback Bug

### Hypothesis 1: Premature Looping of Pattern 0 (the ~9-second 2-channel intro)
* **Symptom**: The song plays the 8.9-second intro riff, but instead of advancing to Pattern 2 (where all 4 channels kick in), it restarts from the beginning. On the Game & Watch's single small speaker, the bass channel (c3) may be faint or lost, making it sound like only 1 channel is looping.
* **Potential causes**:
  1. **Premature loop detection in `jj_render.cpp`**:
     ```cpp
     if (fi.loop_count >= 1 && pass1 < 0)
         pass1 = (long)out.size();
     ```
     If libxmp triggers `fi.loop_count >= 1` on an internal pattern loop/jump effect during conversion, the rendered `.MUS` file gets truncated to just the first pattern (~8.9 seconds) instead of the full 104.8 seconds.
  2. **Loop / rewind triggers in runtime code**:
     In `jj_sound.cpp`:
     ```cpp
     if (done < frames && !jj_mus_rewind(mus))
         break;
     ```
     Or `playMusic()` being re-invoked repeatedly across game loop or scene state transitions.

### Hypothesis 2: Menu Music (`MENUSNG.PSM`) Playing Instead of `SONG0.MUS`
* **Symptom**: `MENUSNG.PSM` is an extremely short loop (~5 seconds) with a simple 1–2 channel melody.
* **Potential cause**: If `jj_plat_find_file("SONG0.MUS")` fails on device (due to pack offset mismatch, casing, or memory mapping bounds), `stopMusic()` leaves or restarts `MENUSNG.PSM`, which keeps looping through gameplay.

### Hypothesis 3: `loop_start` Calculation or ADPCM Seeking Issue
* **Mechanism**:
  In `jj_render.cpp`:
  ```cpp
  long body = pass2 - pass1;
  long restart = pass1 - body;
  if (restart < 0) restart = 0;
  long tail = tail_max < body ? tail_max : body;
  if (restart + tail > pass1) tail = pass1 - restart;
  total = pass1 + tail;
  loop_start = restart + tail;
  ```
  For `SONG0.MUS`, `loop_start` is calculated at `2.0 s` (offset for held notes). If `samples` in the header is read incorrectly or if `f->pos >= f->samples` triggers prematurely on the device, the runtime player seeks back to `loop_start` (2.0 s) and stays in a short loop.

### Hypothesis 4: QSPI Flash Alignment or ADPCM Decoding Stall on Cortex-M7
* On hardware, `.MUS` is decoded directly in place from memory-mapped external flash (QSPI). If 1024-sample ADPCM block reads or predictor calculations encounter bus alignment issues or cache coherency latencies on the STM32H7, `fetch()` / `next_sample()` may produce corrupt output, trigger premature EOF, or wrap.

---

## 3. Local Development & Build Prerequisites

1. **Host build tools** (already present on macOS):
   * `clang`, `clang++`, `make`, `python3`.
   * The headless test harness can be built with `make -C harness`.
2. **For building the Game & Watch hardware binary (`Jazz.bin`)**:
   * **ARM GNU Toolchain**:
     ```bash
     brew install --cask gcc-arm-embedded
     ```
     (Provides `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, etc., required by `sdk/Makefile`).
3. **Optional (multiplatform renderers)**:
   * `zig` (only if using `tools/build_renderers.sh` to cross-compile `jj_render`).

---

## 4. Useful Verification Commands

* **Build the headless host harness**:
  ```bash
  make -C harness
  ```
* **Pack game data and pre-render music**:
  ```bash
  python3 tools/jj_pack.py Jazz.jj ~/roms/dos/JAZZ
  ```
* **Run headless harness with audio recording**:
  ```bash
  ./harness/build/jj_host Jazz.jj -l 0:0 -s "100:A:1,200:A:0,500:A:1,600:A:0,1000:A:1,1100:A:0" -t 35000 -w diamondus_audio.wav
  ```
