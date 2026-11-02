# Handoff: Jazz Jackrabbit GNW — OOM after level-complete cutscene

Date: 2026-11-01  
Repo: `~/code/jazz-jackrabbit-gnw`  
Branch: `experiment`  
Commit: `697a6a81` (`fix: play bonus level after deleting old level to avoid OOM`)  
Status at handoff: **PARTIALLY FIXED and committed**. 
  The OOM is resolved in the harness at both 600 KB and device-sized (575 KB) heap.
  The working tree is clean. Remaining work is device/gwemu verification and cleanup (see checklist).

**2026-11-02 update:** the second OOM (after the bonus level, 64,000 bytes) is
fixed in the harness. See "Continuation 2026-11-02" below; device verification
is still pending.

The original handoff notes and investigation are preserved below for reference.

---

# Continuation 2026-11-02: second OOM (after the bonus level) fixed

## TL;DR

The 64,000-byte OOM after completing the bonus level was the `BONUS.0SC`
cutscene's full-screen animation scratch surface (`320 * 200`) being allocated
while the `JJ1BonusLevel` object was still alive. Same structural problem as the
first OOM: the level was deleted *after* the cutscene instead of before.

Two code changes fix it:

1. `JJ1BonusLevel::releaseResources()` (new) frees the tileset, background,
   sprites, font and palette effects. `JJ1BonusLevel::play()` calls it just
   before `playScene("BONUS.0SC")`. The destructor also calls it (idempotent).
   This mirrors `JJ1Level::releaseResources()` for the normal level-end scene.
2. `Game::playLevel(fileName, intro, checkpoint)` now derives `levelType` from
   `fileName` at entry. Previously the public one-argument overload set it, but
   `Game::play()` calls the three-argument overload directly. After a bonus
   level, `levelType` was left as `LT_JJ1BONUS`, so the next normal level
   (`LEVEL0.001`) was parsed as a bonus map and failed with `E_FILE` (-17).

Also removed the now-dead `JJ1Level::playBonus()` and added a comment in
`Game::play()` explaining that `WON_BONUS` is consumed inside `playLevel()`.

Key files changed:

- `src/oj/game/game.cpp` — `levelType` derivation, `WON_BONUS` comment
- `src/oj/jj1/bonuslevel/jj1bonuslevel.h` — `releaseResources()`
- `src/oj/jj1/bonuslevel/jj1bonuslevel.cpp` — `releaseResources()`, call before
  `BONUS.0SC`, destructor reuse
- `src/oj/jj1/level/jj1level.h` / `jj1level.cpp` — removed `playBonus()`

## Reproduction (pre-fix)

Temporary force hooks (not committed) were used: force the level end + gem on
the first draw, then force the bonus level to win on its first loop iteration.
At `-H 575`:

```text
FORCE level end+gem heap=260512
FORCE bonus win heap=180200
jj_malloc: out of memory (64000 bytes requested, 247016 used / 588792)
jj_host: FATAL at 9353 ms: out of memory (64000 bytes)
```

This is exactly the device symptom: `out of memory (64000 bytes)`.

## Verification (post-fix, forced path)

`-H 575` (device-sized heap), sound and music enabled:

```text
FORCE level end+gem heap=260512
FORCE bonus win heap=180200
jj_host: 50000 ms, 10304 frames presented, audio peak 32768,
         heap used 261312 / peak 351784 / size 588792
```

- No OOM, exit 0.
- No `jj: picture ... not pre-decoded: in RAM` lines (grep count 0).
- After `BONUS.0SC`, the next normal level (`LEVEL0.001`) loaded and ran to the
  50 s limit, confirming the `levelType` fix.

Normal-load regression runs at `-H 575`, 20 s each, no force hooks:
`LEVEL1.000`, `LEVEL0.001`, `LEVEL1.002`, `LEVEL1.001` — all ran with
`heap used 24768 / peak 90952 / size 588792`, no crash.

## Device build

`Jazz.bin` was rebuilt in the Retro-Go SD builder container from the current
source:

```text
text 131304  data 528  bss 18616
segment[0]: region=ram_emu code=131832B bss=18616B
Jazz.bin: 138578 bytes
```

New device heap = 741,376 - 131,832 - 18,616 = **590,928 bytes**, slightly
larger than the harness `-H 575` heap (588,792) used above, so the harness
result is conservative.

## Remaining checklist

- [ ] **Device/gwemu playthrough**: `Jazz.bin` is rebuilt; play through a bonus
  level on gwemu and confirm the `BONUS.0SC` cutscene and the following level
  load. (The harness path is verified; actual device play still needs a human.)
- [ ] **Automate episode 1 transitions** end-to-end (still open).
- [x] **`not pre-decoded` check**: no bake misses observed on the forced
  level-end + bonus + next-level path.
- [x] **Dead code cleanup**: `JJ1Level::playBonus()` removed.
- [x] **`WON_BONUS` in `Game::play()` loop**: documented; `playLevel()` consumes
  it and returns `WON`.
- [ ] **Holiday Hare / shareware data**: retest with alternate data sets if
  available.

---

# Continuation 2026-11-01 - later: device trial

Human user rebuilt Jazz.bin with latest source, then ran the game on gwemu (./run_gwemu.sh).
* Completed level 1 and 2
* collected gem
* saw the "level cleared" cutscene
* bonus level loaded and was playable
* collected the 30 "gems" in bonus level in order to exit the level
* got a new OOM: "Jazz Jackrabbit error: out of memory (64000 bytes)" that booted me back to retro-go-sd launcher

## Remaining checklist

- [ ] investigate the new OOM (after bonus), possibly same type of issue (trying to load next intro/level without unloading current bonus level)
- [ ] automate all transitions of episode 1 (intro 1 -> level 1 -> level 2 -> bonus -> intro 2 -> level 1 -> level 2 -> bonus -> intro 3 -> level 1 -> level 2 -> bonus)
- [ ] **`not pre-decoded` check**: during a real (non-forced) transition, grep for `jj: picture ... not pre-decoded: in RAM` in the harness output. A bake miss may still lurk for some cutscene or next-level picture.
- [ ] **Dead code cleanup**: `JJ1Level::playBonus()` is no longer called from anywhere. Either remove it (and its declaration in `jj1level.h`) or leave it as an internal utility. Not urgent.
- [ ] **`WON_BONUS` in `Game::play()` loop**: the loop currently never sees `WON_BONUS` (it's consumed inside `playLevel`). If this invariant ever breaks the behaviour is harmless (it falls through as `> 0`, treated like LOST in the non-bonusmap branch), but adding an assertion or comment would make it explicit.
- [ ] **Holiday Hare / shareware data**: retest with alternate data sets if available.

---

# Continuation 2026-11-01: fix implemented

## TL;DR of fix

The structural fix (hypothesis 5 from the previous continuation) was implemented and verified:

- `WON_BONUS = 3` added to `src/oj/level/level.h`.
- `JJ1Level` gained `pendingBonusFile` (private `char*`) and `takePendingBonusFile()` (public, transfers ownership).
- `JJ1Level::play()`: when the level is won and the gem is collected, instead of calling `playBonus()` inline, the BONUSMAP filename is computed, stored in `pendingBonusFile`, and `WON_BONUS` is returned. `setLevel()` for the next normal level is still called before the return.
- `Game::playLevel()`: handles `WON_BONUS` by calling `takePendingBonusFile()`, then `delete level` (old level freed, heap coalesced), then `playLevel(bonusFile)`. Errors from the bonus propagate; WON/LOST/E_NONE are normalised to WON.
- The `releaseResources()` change (frees tileset, sprites, panel, font before the cutscene) was kept and is included in the commit.

Key files changed:

- `src/oj/level/level.h` — `WON_BONUS`
- `src/oj/jj1/level/jj1level.h` — `pendingBonusFile`, `takePendingBonusFile()`
- `src/oj/jj1/level/jj1level.cpp` — `releaseResources()`, updated `play()`, `takePendingBonusFile()`
- `src/oj/game/game.cpp` — `WON_BONUS` handling in `playLevel()`

## Harness verification results

All tests run with `harness/build/jj_host Jazz.jj` (rebuilt at commit `0991242`).

### Gem path — 600 KB harness heap (default `-H 600`)

```
TRACE Game::playLevel file="LEVEL1.000" heap=25032 peak=90952
TRACE JJ1Level::load  file="LEVEL1.000" heap=212288 peak=212288
TRACE Game::playLevel file="BONUSMAP.000" heap=26400 peak=351784
jj_host: 45000 ms, 10147 frames, heap used 180200 / peak 351784 / size 614392
```

BONUSMAP.000 constructed with heap=26,400 (old level fully freed). Peak 351,784 / 614,392. **No OOM.**

Before fix: OOM at 61,440 bytes, used=414,112 / 614,392.

### Gem path — device-sized heap (`-H 575`, size=588,792)

```
TRACE Game::playLevel file="BONUSMAP.000" heap=26400 peak=351784
jj_host: 45000 ms, heap used 180200 / peak 351784 / size 588792
```

Peak 351,784 / 588,792. **No OOM on device-sized heap either.**

### No-gem normal transitions — 600 KB heap

```
TRACE Game::playLevel file="LEVEL1.000" heap=25032 peak=90952
TRACE JJ1Level::load  file="LEVEL1.000" heap=212288 peak=212288
TRACE Game::playLevel file="LEVEL0.001" heap=26376 peak=351784
TRACE JJ1Level::load  file="LEVEL0.001" heap=213632 peak=351784
TRACE Game::playLevel file="LEVEL1.001" heap=26376 peak=351784
TRACE JJ1Level::load  file="LEVEL1.001" heap=213632 peak=351784
TRACE Game::playLevel file="LEVEL0.002" heap=26376 peak=351784
TRACE JJ1Level::load  file="LEVEL0.002" heap=213632 peak=351784
jj_host: 30000 ms, 3549 frames, heap used 250440 / peak 351784 / size 614392
```

Heap returns to ~26,376 at every `Game::playLevel` entry. No monotonic growth. **No leak, no regression.**

## Remaining checklist

- [ ] **gwemu / device verify**: run `./run_gwemu.sh`, play normally through two levels with a gem, confirm third level loads. Capture `jj:` heap lines from gwemu.log; expect no OOM message and peak well under 589,088.
- [ ] **Rebuild Jazz.bin for device**: `make -C build/homebrew -j8` (or whatever the device build target is), then re-stage with `run_gwemu.sh`. The harness binary is separate from `Jazz.bin`.
- [ ] **`not pre-decoded` check**: during a real (non-forced) transition, grep for `jj: picture ... not pre-decoded: in RAM` in the harness output. A bake miss may still lurk for some cutscene or next-level picture.
- [ ] **Dead code cleanup**: `JJ1Level::playBonus()` is no longer called from anywhere. Either remove it (and its declaration in `jj1level.h`) or leave it as an internal utility. Not urgent.
- [ ] **`WON_BONUS` in `Game::play()` loop**: the loop currently never sees `WON_BONUS` (it's consumed inside `playLevel`). If this invariant ever breaks the behaviour is harmless (it falls through as `> 0`, treated like LOST in the non-bonusmap branch), but adding an assertion or comment would make it explicit.
- [ ] **Holiday Hare / shareware data**: retest with alternate data sets if available.

---

# Earlier investigation (2026-11-01 Continuation 1): reproduction and root cause

## TL;DR

The OOM is **not a leak**. It happens on the **gem-collected bonus-level path**:

1. The level is completed, the end cutscene (`endlevel.0sc`) plays and is freed.
2. `JJ1Level::playBonus()` then constructs a `JJ1BonusLevel` **while the completed level is still alive**.
3. `JJ1BonusLevel` needs one ~137 KB heap object (`grid[256][256]` is inline in the class) plus ~61 KB and ~65 KB temporary RLE decode buffers.
4. The heap has enough **total** free bytes (200-247 KB) but no **contiguous** block large enough for the 61,440 / 65,536-byte temporary. The allocator is first-fit with coalescing and cannot compact.
5. `jj_malloc` returns NULL and the port fatals with `out of memory`.

A partial fix that frees the finished level's graphical resources before the cutscene/bonus (`JJ1Level::releaseResources()`) only recovers ~46 KB, because most level pictures are baked and live in flash. It moves the failure from 61,440 bytes to 65,536 bytes but does **not** fix the crash on its own.

## Device heap size

From `build/homebrew/jj_core.elf` (matches `Jazz.bin`: 140,402 bytes) and `ld/gnw_ram_emu.ld`:

- `__RAM_EMU_LENGTH__` = 1024K - 300K = 741,376 bytes (`0xB5000`)
- core segment 0: code = 133,656, bss = 18,632 (also encoded in the `GWHB` header of `Jazz.bin`)
- device heap = 741,376 - 133,656 - 18,632 = **589,088 bytes**

The harness default is 614,392 bytes (`-H 600`). The harness is therefore slightly *larger* than the device; reproducing there is conservative. `-H 575` approximates the device heap.

## Reproduction (pre-fix)

Force the level-end path and the gem on every draw, then advance cutscenes with periodic `A` presses:

```bash
SCRIPT=$(python3 - <<'PY'
ev = [(500,'A',1),(600,'A',0)]
for t in range(2000, 90000, 1200):
    ev += [(t,'A',1),(t+100,'A',0)]
print(','.join(f'{a}:{b}:{c}' for a,b,c in ev))
PY
)

lldb --batch -o "command script import /tmp/jj_trace.py" -o run -o quit -- \
  harness/build/jj_host Jazz.jj -l 0:1 -t 30000 -m -n -o /tmp/jjend -s "$SCRIPT"
```

With gem set, crash was immediate on the first bonus level:

```text
TRACE Game::playLevel file="LEVEL1.000" heap=25032
TRACE JJ1Level::load  file="LEVEL1.000" heap=212280
TRACE Level::playScene heap=260728
TRACE JJ1Level::playBonus heap=260344
TRACE Game::playLevel file="BONUSMAP.000" heap=260368
TRACE JJ1BonusLevel ctor file="BONUSMAP.000" heap=397632
TRACE big malloc size=61440
  BT 3 File::loadRLE(int, bool) file.cpp:441
  BT 4 JJ1BonusLevel::loadTiles(char*) jj1bonuslevel.cpp:160
  BT 5 JJ1BonusLevel::JJ1BonusLevel(...) jj1bonuslevel.cpp:246
  BT 6 Game::playLevel(...) game.cpp:222
  BT 7 JJ1Level::playBonus() jj1level.cpp:693
jj_malloc: out of memory (61440 bytes requested, 414112 used / 614392)
jj_host: FATAL at 9262 ms: out of memory (61440 bytes)
```

Free list at failure: `blocks=95 total=200280 largest=42408` — fragmentation was the direct failure.

## Key files

- `src/port/jj_alloc.c` — game heap, OOM message.
- `src/port/jj_main.c` — device heap setup, stats logging, save dir.
- `src/port/jj_bake.c` — baked surface lookup/recording.
- `src/port/jj_sdl.cpp` — surface creation, `jj_sdl_create_baked`, fatal OOM messages.
- `src/oj/io/gfx/video.cpp` — `Video::createSurface`, `destroySurface`, `bakeSurface`.
- `src/oj/jj1/level/jj1level.cpp` — `JJ1Level::play`, `releaseResources`, `takePendingBonusFile`, destructor.
- `src/oj/jj1/level/jj1levelload.cpp` — level loading, panel/tiles/sprites, scene/music names.
- `src/oj/jj1/scene/jj1scene.cpp` — scene animation scratch/teardown.
- `src/oj/jj1/scene/jj1sceneload.cpp` — scene data/script loading.
- `src/oj/game/game.cpp` — `Game::playLevel` (WON_BONUS handling), `Game::play`.
- `src/oj/level/level.cpp` — `Level::playScene`, `Level::~Level`.
- `src/oj/level/level.h` — `WON`, `LOST`, `WON_BONUS` return codes.
- `harness/jj_host.c` — harness options and virtual clock.
- `harness/Makefile` — `LIVE=1`, `SAN=1` builds.
- `tools/live_report.py` — live heap report.
- `run_gwemu.sh` — device/emulator staging and QMP launch.

---

# Original handoff notes (2026-10-xx): initial investigation

## Symptom

On the Game & Watch (and reproducible in `gwemu`), the player can clear the first two levels. After the level-complete cutscene, the game crashes with an out-of-memory error instead of loading the next level.

The device log should contain a line from `src/port/jj_alloc.c` like:

```text
jj_malloc: out of memory (N bytes requested, USED used / TOTAL total)
```

or a surface-specific fatal from `src/port/jj_sdl.cpp`:

```text
out of memory (surface WxH)
out of memory (surface copy)
out of memory (palette)
```

## Assets / pack

- DOS game data: `~/roms/dos/jazz`; the actual DOS files are under `~/roms/dos/jazz/drives/c/jazz`.
- `~/roms/dos/jazz/openjazz.log` is from the desktop OpenJazz build, not the GNW port; it is not the crash log.
- Repo pack: `Jazz.jj`, 47,108,264 bytes, 323 entries.
- `SURFACES.BAK` inside the pack: 11,772,836 bytes.
- `Jazz.bin` at handoff: 140,402 bytes (built 2026-10-28).
- Pack build tool: `tools/jj_pack.py`; it runs the harness with `-B` to bake pictures and `-R` to render music.

## Relevant architecture

- Game heap: `src/port/jj_alloc.c`, a ~600 KB boundary-tag allocator placed in RAM_EMU after the binary. First-fit free list, coalescing on free, no fragmentation metric.
- Heap initialization on device: `src/port/jj_main.c` near `ram_get_free_size()` / `jj_heap_init()`.
- Pictures are normally used in place from the pack:
  - `Video::createSurface()` in `src/oj/io/gfx/video.cpp` calls `jj_sdl_create_baked()` first.
  - `src/port/jj_bake.c` hashes pixels and looks them up in `SURFACES.BAK`.
  - `src/port/jj_sdl.cpp` prints `jj: picture WxH not pre-decoded: in RAM` when a large picture is not found in the bake and must live in RAM.
- Cutscene animations allocate a writable 320x200 scratch surface in RAM:
  - `JJ1SceneAnimation::JJ1SceneAnimation()` in `src/oj/jj1/scene/jj1scene.cpp` around line 96:
    `scratch = video.createSurface(nullptr, SW, SH);`
  - 320 * 200 = 64,000 bytes per animation. One animation is active at a time, but every animation object gets its own scratch.

## Level transition code path

- `JJ1Level::play()`: `src/oj/jj1/level/jj1level.cpp`
  - Level won condition: `releaseResources()`, cutscene, then (if gem) sets `pendingBonusFile` and returns `WON_BONUS`; else calls `setLevel()` and returns `WON`.
- `Game::playLevel()`: `src/oj/game/game.cpp`
  - Constructs `JJ1Level`, calls `level->play()`.
  - If `WON_BONUS`: grabs `pendingBonusFile`, `delete level`, plays bonus level, returns `WON`.
  - Otherwise: `delete level`, returns ret.
- `Game::play()`: loops on `playLevel()` and `setLevel()` until the episode/game ends.
- `Level::playScene()`: `src/oj/level/level.cpp` line 117. Deletes `paletteEffects`, constructs `JJ1Scene`, plays it, deletes it.

## Harness usage notes

### `-l` argument order is actually `LEVEL:WORLD`

Despite the README saying `-l world:level`, the code path is level:world. Examples:

```bash
# loads LEVEL1.000
harness/build/jj_host Jazz.jj -l 0:1 -t 3000 -m -n -o /tmp/jjlevels -s "500:A:1,600:A:0"

# loads LEVEL1.002
harness/build/jj_host Jazz.jj -l 2:1 -t 3000 -m -n -o /tmp/jjlevels -s "500:A:1,600:A:0"
```

### lldb Python callback technique

```python
bp = target.BreakpointCreateByName("JJ1Level::load", None)
bp.SetScriptCallbackBody('''
name = frame.FindVariable("fileName").GetSummary()
heap = frame.EvaluateExpression("(unsigned)jj_heap_used()").GetValueAsUnsigned()
print("TRACE load file=%s heap=%u" % (name, heap), flush=True)
return False
''')
```

- `localPlayer` is a global, not a member of `JJ1Level`.
- `gem` is private; it is at byte offset **1440** of `JJ1LevelPlayer`.
- Forcing gem: `*(volatile unsigned char*)((char*)localPlayer->getJJ1LevelPlayer() + 1440) = 1`
- Forcing level end: `this->setStage((LevelStage)2)` from a `JJ1Level::draw` breakpoint.

### `-B` bake mode is not a useful leak test

`-B` runs in recording mode so every surface is heap-allocated. Do not use `-B -H 600` as evidence of the device bug.
