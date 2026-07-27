# Status and Roadmap

This document records where the ESP32-S3 port of OpenLara stands, phase by phase, and
specifies each remaining piece of work concretely enough that someone (including future us)
could pick it up cold. Dates and commits are real; numbers are measured on the physical
Waveshare ESP32-S3-Touch-LCD-2 unless marked otherwise. The memory design that made
Phase 3 possible is documented separately in [05-memory-map.md](05-memory-map.md).

Branch: `esp32s3` on the fork `github.com/SoftwareEscarlata/OpenLara`.

## 1. Phase table

| Phase | Scope | Status | Date | Commit | Evidence |
|---|---|---|---|---|---|
| 1 | Windows simulator (`__ESP32_WIN__`, byte-exact target profile) | **DONE** | 2026-07-27 | `b278f9e` | TR1 running at 64 fps in `OpenLara_esp32sim.exe` |
| 2 | Display pipeline on hardware (esp_lcd, ST7789, DMA bands) | **DONE** | 2026-07-27 | `0f3db80` | Full-frame flush measured 16.37 ms at 80 MHz SPI (wire floor 15.36 ms, 6% overhead) |
| 3 | Engine on hardware (flash mmap, PSRAM, `EWRAM_COLD`) | **DONE** | 2026-07-27 | `0d3816f` | Title screen 62–63 fps (display ceiling); in-game GYM 48–50 fps (original game: 30) |
| 4a | Input: buttons | **Firmware DONE, wiring pending** | 2026-07-27 | — | GPIO table + serial input built and flashed-ready; header P1 not yet soldered |
| 4b | Audio output | **Pending** | — | — | Mixer compiles and runs; no output device yet (see 3.1) |
| 5 | Performance (IRAM, core 1, profile relaxation) | **Pending** | — | — | see 3.6 |
| 6 | Polish (TITLE.SCR, NVS saves, SD levels) | **Pending** | — | — | see 3.2–3.4 |

All three commits landed in a single working session on 2026-07-27. Hours of runtime on
the board and in the simulator without a crash or panic.

### What "done" means per phase

- **Phase 1** produced the reference build: same `MODE13`/320x240/PKD defines as the
  target, i686 MinGW (the engine is 32-bit-pointer-only), the `src/platform/esp32/rasterizer.h`
  based on `dos/rasterizer.h` with three upstream bugs fixed (`VRAM_WIDTH` defined nowhere,
  missing `R = L;` in `rasterizeF_c`, four empty sprite/line/fill stubs ported from GBA).
- **Phase 2** produced `src/platform/esp32/idf/main/display_esplcd.c`: 256-entry
  pre-byteswapped LUT, 48-line bands sized so each band is exactly one SPI transaction
  (30,720 B, just under the S3's 32,768 B/transaction cap), two ping-pong DMA buffers
  gated by a counting semaphore, expansion overlapped with the in-flight transfer.
- **Phase 3** produced `src/platform/esp32/idf/main/game_main.cpp` (the platform shell),
  the OLVL flash container + `make_levels.py`, and the memory work. The only engine-code
  change Xtensa required in ~100k lines was `#include <new>` on `__ESP32__`
  (`src/fixed/common.h:344-347`).
- **Phase 4a**: `board_pins.h` maps all 10 buttons to header P1 (active-low, internal
  pullups; IO16/IO21 have 4.7K hardware pullups); `inputUpdate` in `game_main.cpp` already
  ORs three sources — serial-over-USB keys, the physical pad table `sButtons`, and BOOT
  as START+A. What is missing is solder, not software. Caveat carried in the pin comments:
  the camera must stay unplugged (IO2/4/6/7/8/10 are DVP pins), and IO19/20 stay clear
  (USB).

## 2. How to run what exists today

- **Simulator:** `src/platform/esp32/win/` — `make` with `C:/msys64/mingw32/bin` on PATH
  (i686 g++ mandatory; the Makefile refuses x86_64). Debug keys: T fast-forward, R slow-mo,
  1–4 weapon cheats.
- **Board:** `src/platform/esp32/idf/build.ps1`, then `flash.ps1` (add `-Levels` the first
  time to write the 9.76 MB OLVL image at 0x310000, ~84 s at 921600 baud).
- **Play over serial:** `python -m serial.tools.miniterm COM6 115200` — w/s/a/d d-pad,
  x=A, z=B, q=L, e=R, Enter=START, space=SELECT. Each char holds its key bit for 8 frames
  (serial has no key-up event). In-game chords are the GBA scheme from `src/fixed/lara.h`:
  A=action, A+L=weapon, B=jump, B+L=roll, R=walk, R+L=look.

## 3. Pending work, as mini-specs

### 3.1 Audio output (Phase 4b)

**Current state.** The mixer is fully alive: `src/platform/gba/sound.cpp` is compiled
into the firmware (`idf/main/CMakeLists.txt:28`), `TRACKS_AD4` points into the flash mmap,
and the `__ESP32__` branch of `common.h:456-463` already defines the sound profile:
`SND_SAMPLES 1024`, `SND_OUTPUT_FREQ 22050`, 8-bit unsigned encoding
(`SND_ENCODE(x) = (x) + 128`). `sndFill(int8*)` produces a mixed mono buffer of music
(ADPCM4) plus up to `SND_CHANNELS` PCM effects into `soundBuffer[2*SND_SAMPLES+32]`.
Nothing consumes it on the board yet. The Windows harness proves the whole chain: it
double-buffers `soundBuffer` through waveOut, calling `sndFill` on each `MM_WOM_DONE`
(`src/platform/esp32/win/main.cpp:114-147`) — that is exactly the shape the ESP32 version
needs, with the DMA-done callback playing the role of `MM_WOM_DONE`.

**Decision to make: MAX98357A (I2S) vs PWM/LEDC.**

| | MAX98357A I2S | PWM (LEDC / sigma-delta) |
|---|---|---|
| BOM | ~2 USD module + speaker | Zero (buzzer) or transistor + speaker |
| Quality | Real DAC + 3W class-D amp; 22050 Hz 8-bit source is the limiting factor, not the link | Carrier whine and ~6–7 effective bits; marginal for music |
| CPU | I2S peripheral DMA, near-zero CPU | Timer/duty updates at sample rate, or sigma-delta with RC filter |
| Pins | 3 (BCLK, LRCLK, DIN) | 1 |
| Software | `i2s_std` channel at 22050 Hz, expand `int8+128` mono to 16-bit in the write path | ISR or dedicated task at 22 kHz |

Recommendation: MAX98357A. The mixer's output format was designed for a real DAC path
(the GBA has one), the I2S peripheral costs no CPU we would rather spend on the
rasterizer, and 3 GPIOs are findable — with the camera unplugged, DVP pins in the
IO11–IO15 range are the natural candidates (exact choice to be verified against the
schematic at wiring time; header P1 itself is fully claimed by the buttons).

Implementation sketch: task pinned to core 1 (reserved for this since Phase 3 —
`game_main.cpp:287-289`), blocking loop of `sndFill` into one half of `soundBuffer` then
`i2s_channel_write` of the other, 1024 samples = 46.4 ms per buffer. Volume/mute already
exist in the engine (`gSettings.audio_sfx/audio_music` gate the mixer).

### 3.2 TITLE.SCR regeneration (Phase 6)

**Current state.** The title/inventory background is black. The cause is precise:
`renderBackground` copies exactly `FRAME_WIDTH * FRAME_HEIGHT` bytes into `fb`
(`src/platform/gba/render.iwram.cpp:1189-1192`), i.e. it expects a raw 8bpp 320x240 image
of exactly **76,800 bytes**. The GBA asset in the repo is 240x160 (38,400 B) — wrong
size, would read past its end — so `game_main.cpp:236-237` substitutes a zeroed PSRAM
buffer:

```c
// no 320x240 TITLE.SCR asset yet -> black background (PSRAM, zeroed)
TITLE_SCR = heap_caps_calloc(1, FRAME_WIDTH * FRAME_HEIGHT, MALLOC_CAP_SPIRAM);
```

**Spec.** Run the asset packer (`src/platform/gba/packer/`) against the original TR1 data
with a 320x240 output profile to emit a new `TITLE.SCR` (raw 8bpp indexed pixels in the
level palette, no header, 76,800 B exactly). Add the file to `make_levels.py`'s input
list so it lands in the OLVL container; in `gameTask`, replace the `calloc` with a
`levelsFind("TITLE.SCR")` + PSRAM copy (it must be a copy, not the mmap pointer, only if
anything mutates it — `renderBackground` reads it, so the mmap pointer is actually
sufficient; the copy is optional). Note the packer requires the original TR1 assets,
which are not in the repo.

### 3.3 Saves and settings in NVS (Phase 6)

**Current state.** All five persistence hooks are stubbed to `return false` in
`game_main.cpp:93-97` (`osSaveSettings`, `osLoadSettings`, `osCheckSave`, `osSaveGame`,
`osLoadGame`), so the game runs on defaults and passcards progress dies with the power.
The `nvs` partition already exists in `idf/partitions.csv`.

**Spec.** Mirror the file formats the desktop harness uses (`src/platform/gba/main.cpp:34-90`,
`__GBA_WIN__` branch):

- *settings*: the raw `gSettings` struct, whose first byte is `gSettings.version`; on
  load, reject if the version byte differs (the struct layout changed).
- *savegame*: `gSaveGame` (a header starting with a `uint32` version and containing
  `dataSize`) followed by `gSaveData[gSaveGame.dataSize]` — bounded by
  `SAVEGAME_SIZE = 8 KB` total (`common.h:1782`).

In NVS terms: two blob keys (e.g. `"settings"`, `"save0"`) in one namespace, written with
`nvs_set_blob` + `nvs_commit`. `osCheckSave` = `nvs_get_blob` of the first 4 bytes and a
version compare. One subtlety: `gSaveData` is `EWRAM_COLD`, i.e. in PSRAM — fine for NVS
APIs, which take ordinary pointers. Flash wear is a non-issue at human save frequency.

### 3.4 SD card levels (Phase 6)

**Current state.** Not started. The hardware path is already prepared: the microSD sits
on the same SPI2 bus as the LCD, `PIN_SD_MISO 40` is wired into the bus config
(`display_esplcd.c:64`) and `PIN_SD_CS 41` is parked high from boot so the card never
drives MISO (`display_esplcd.c:58-59`).

**Spec.** Mount SDSPI + FATFS on the shared bus; extend `levelsFind`/`osLoadLevel` with a
filesystem fallback (look for `<NAME>.PKD` on the card before the OLVL container). Two
constraints: (1) card I/O must happen only during level load, never concurrently with
`displayFlush` — the bus is shared and reads through the GPIO matrix cap at 40 MHz;
(2) adding levels beyond LEVEL2 is *not* only a data problem — see 3.5.

### 3.5 Known limitations (honest list)

- **Only 4 levels exist in `gLevelInfo`** upstream (`src/fixed/common.cpp:47-73`): TITLE,
  GYM, LEVEL1, LEVEL2. Everything else — LEVEL3A through the pyramid, and all cutscenes
  (CUT1–CUT4) — is commented out in the table. Enabling more levels means uncommenting
  entries, packing the corresponding `.PKD`s (original assets required), and testing;
  cutscene support in the potato engine is untested territory for this port.
- **No cutscenes** (same root cause as above).
- **Inventory/title background is black** until 3.2 lands.
- **No audio output** until 3.1 lands (the mixer runs; you just cannot hear it).
- **No persistence** until 3.3 lands.
- **Serial input is edge-triggered with an 8-frame hold** — fine for testing, not for
  play; real buttons (4a wiring) are the fix.
- **Single player, GBA chord scheme** — inherent to the potato profile (`MAX_PLAYERS 1`).
- **TE not routed on this board**: no tear sync possible, we rely on queued DMA bands;
  no visible tearing observed in hours of running, but it is unsynchronized by design.

### 3.6 Performance program (Phase 5)

Current numbers: 62–63 fps title (display-bound: 16.37 ms flush = ~61 fps ceiling),
48–50 fps in GYM (original game logic rate: 30 Hz — we render faster than the game
thinks). The levers, in intended order, all to be applied one at a time with
`PROFILE_FRAMETIME` measurements:

1. **`IRAM_ATTR` on the rasterizer inner loops** (`rasterize*`, transform, flush paths) —
   removes I-cache misses against flash on the hottest code.
2. **Core 1 offload** — audio mixing (3.1) and potentially the palette expansion of
   `displayFlush`; core 1 is idle today by design.
3. **Relax the potato profile** (`common.h:229-238`). The `__ESP32__` block currently
   copies the proven GBA cuts: `MAX_ENEMIES 3`, `VIEW_DIST (10 << 10)`,
   `HIDE_CORPSES (30*10)`, `LOD_TRAP_FLOOR`, `NO_STATIC_MESH_PLANTS`, `FAST_HITMASK`.
   The S3 has CPU headroom the GBA never had; candidates in order of player-visible
   value: `VIEW_DIST` up (fog pushes back; also grows `gOT` slightly), `MAX_ENEMIES` up
   (each +1 costs ~8.3 KB of `enemiesExtra`, which is in PSRAM, so cheap),
   drop `NO_STATIC_MESH_PLANTS`, drop `LOD_TRAP_FLOOR`, drop `FAST_HITMASK` (restores
   per-sphere hit detection). Each change: measure GYM and LEVEL1 fps before keeping.
4. **`SPIRAM_RODATA` / `SPIRAM_FETCH_INSTRUCTIONS` experiment** — one-line sdkconfig
   change that moves flash traffic to the PSRAM side of the SPI0 arbiter (see
   [05-memory-map.md](05-memory-map.md), section 9).
5. **PSRAM at 120 MHz** — exists but is officially experimental and
   temperature-sensitive; last on the list, and only with soak testing.

## Key takeaways

- Phases 1–3 plus the 4a firmware were completed in one session on 2026-07-27
  (commits `b278f9e`, `0f3db80`, `0d3816f`): the full TR1 potato engine runs on the
  physical board at 48–63 fps, above the original game's 30.
- The Windows simulator is the project's safety net: byte-exact defines, same rasterizer,
  64 fps, and it is where the remaining features (audio consumption pattern, save
  formats) already have working reference implementations to copy.
- Everything pending is bounded and specified: audio needs a hardware decision
  (MAX98357A recommended) but the mixer already runs; TITLE.SCR is a 76,800-byte packer
  output; saves are two NVS blobs with formats lifted from `gba/main.cpp`; SD levels are
  a FATFS fallback with a bus-sharing rule.
- The honest ceiling on content is upstream, not this port: `gLevelInfo` ships four
  levels and no cutscenes; going further requires original assets and the packer.
- Performance work is deliberately last and deliberately empirical: measure with
  `PROFILE_FRAMETIME`, change one lever at a time, keep what the numbers justify.
