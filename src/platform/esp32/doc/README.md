# OpenLara on the ESP32-S3 — Documentation Index

This directory documents a working port of **OpenLara** (XProger's open-source Tomb Raider 1
engine) to the **Waveshare ESP32-S3-Touch-LCD-2**, a ~$20 board with a 240 MHz dual-core
Xtensa LX7, 8 MB of octal PSRAM, 16 MB of flash and a 2" 320x240 ST7789 SPI display. The
port runs the full TR1 game — title screen, inventory, GYM and the first levels — at
**48-63 fps**, faster than the original PlayStation game's 30 fps. It was possible because
OpenLara contains a second, often-overlooked engine: `src/fixed/`, a pure fixed-point
(`FIXED_SHIFT 14`), 8-bit-paletted software renderer written to run full TR1 on a Game Boy
Advance (16.78 MHz ARM7TDMI, 288 KB RAM). The port maps the GBA's memory model onto the
ESP32-S3 — cart ROM becomes memory-mapped flash, EWRAM becomes octal PSRAM (via a new
`EWRAM_COLD` macro), IWRAM becomes internal SRAM — and adds a DMA display pipeline that
turns the SPI bottleneck into a 16.37 ms/frame ceiling. This README is the front door:
it gives you the results, the three ways to run the port, and a map of the ten detailed
documents in this set.

## Results at a glance

All numbers measured on the physical board (USB serial, COM6) unless noted.

| Metric | Measured value |
|---|---|
| Title screen frame rate | 62-63 fps (display-limited) |
| In-game frame rate (GYM) | 48-50 fps (original game: 30 fps) |
| Windows simulator frame rate | 64 fps |
| Full-frame display flush (320x240 RGB565, 80 MHz SPI) | 16.37 ms (wire floor 15.36 ms, ~6% overhead) |
| Level load, TITLE.PKD 306 KB, flash to PSRAM | 16 ms |
| Internal heap free after full game init | 33 KB (largest block 17 KB) |
| Internal RAM freed by moving cold data to PSRAM (`EWRAM_COLD`) | ~130 KB (before it, `.dram0.bss` overflowed by 2528 bytes) |
| Game logic tick | 30 Hz (33 ms), unchanged from GBA |
| Levels image flashed (4 levels + soundtrack) | 9.76 MB, ~84 s at 921600 baud |

Where these numbers come from and what they mean: see `04-display-pipeline.md`,
`05-memory-map.md` and `08-performance.md`.

## Quick start

There are three ways to interact with the port, in increasing order of hardware required.

### A. Run the Windows simulator (no hardware needed)

The simulator (`win/`) is byte-exact with the target: same defines, same 320x240 8bpp
framebuffer, same rasterizer, same PKD level files. It is the reference for "is this an
engine bug or a platform bug?". Full details in `03-windows-simulator.md`.

Prerequisites (one-time), from an MSYS2 shell:

```
pacman -S mingw-w64-i686-gcc mingw-w64-i686-make
```

The compiler **must be i686 (32-bit)** — the engine stores pointers in `uint32`
(PKD offset fixups, rasterizer color smuggling), so an x86_64 g++ fails to compile,
and the Makefile refuses it explicitly. Build and run:

```
# any shell with C:\msys64\mingw32\bin on PATH and cp/mkdir available
# (MSYS2 MinGW 32-bit shell or Git Bash both work)
cd src/platform/esp32/win
mingw32-make
./OpenLara_esp32sim.exe
```

`C:\msys64\mingw32\bin` **must** be on `PATH` when compiling — without it g++ exits 1
with zero output (DLL load failure). The exe itself is linked `-static` and runs anywhere.

Simulator controls (from `win/main.cpp`):

| Key | GBA button | In-game meaning (GBA chord scheme) |
|---|---|---|
| Arrow keys | D-pad | move / turn |
| S | A | action; S+Q = draw weapon |
| A | B | jump; A+Q = roll |
| W | R | walk; W+Q = look |
| Q | L | chord modifier |
| Enter | START | inventory / confirm |
| Space | SELECT | — |
| T (hold) | — | fast-forward x10 (debug) |
| R (hold) | — | slow-motion /10 (debug) |
| 1 / 2 / 3 / 4 | — | weapon cheat: pistols / magnums / uzis / shotgun (in level only) |

### B. Build and flash the firmware

Requires ESP-IDF v5.5.4 installed at `C:\Espressif5.5` (the scripts drive cmake+ninja
directly — no `idf.py`; the environment recipe is inside `build.ps1` and explained in
`07-build-system.md`).

```
cd src/platform/esp32/idf
powershell -ExecutionPolicy Bypass -File build.ps1              # configure + build
powershell -ExecutionPolicy Bypass -File flash.ps1 -Levels      # first flash: app + level data
powershell -ExecutionPolicy Bypass -File flash.ps1              # later flashes: app only (fast)
```

Notes:

- `flash.ps1` auto-detects the serial port; pass it explicitly if needed:
  `flash.ps1 COM6 -Levels`. Close any serial monitor first — esptool cannot open a
  port that a terminal is holding.
- `-Levels` packs `TITLE/GYM/LEVEL1/LEVEL2.PKD` + `TRACKS.AD4` (from
  `src/platform/gba/data/`) into an `OLVL` container via `make_levels.py` and writes it
  at flash offset `0x310000` (the `levels` partition). It is 9.76 MB and takes about
  84 seconds — you only need it once, or when the level data changes.
- `build.ps1 clean` wipes the build directory.

### C. Play it

**From the PC keyboard over USB serial** (no wiring needed) — the firmware reads
characters from the USB-Serial-JTAG port and each keypress holds the corresponding
button for 8 frames (serial has no key-up events):

```
C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe -m serial.tools.miniterm COM6 115200
```

(Any Python with `pyserial` works; the esptool venv above already has it.)

| Serial key | GBA button | In-game meaning |
|---|---|---|
| w / s / a / d | D-pad up / down / left / right | move / turn |
| x | A | action; x+q = draw weapon |
| z | B | jump; z+q = roll |
| e | R | walk; e+q = look |
| q | L | chord modifier |
| Enter | START | inventory / confirm |
| Space | SELECT | — |

The on-board **BOOT button (GPIO0)** is wired as an emergency START+A, so you can start
a game with no PC and no wiring at all.

**With physical buttons**: a full 10-button pad connects to header P1 (active-low,
internal pull-ups, GND on pin 13). The pin table, wiring guide and the constraint that
the camera module must be unplugged (shared DVP pins) are in `02-hardware.md`.

## Documentation map

Read in order for the full story, or jump to the topic you need:

1. **`01-architecture.md`** — The key insight: OpenLara's two engines, why `src/fixed/`
   (the GBA "potato" engine) is the portable one, and the GBA-to-ESP32-S3 conceptual
   memory mapping that shaped every later decision.
2. **`02-hardware.md`** — The Waveshare ESP32-S3-Touch-LCD-2 in detail: schematic-verified
   pinout (LCD, touch, IMU, SD, camera), the P1 button header wiring guide, and the
   board-specific traps (no LCD reset pin, backlight boots dark, shared SPI bus).
3. **`03-windows-simulator.md`** — The `__ESP32_WIN__` build: why a byte-exact 32-bit
   simulator was built first, how it mirrors `gba/main.cpp`, and every toolchain gotcha
   (i686-only, `-U__WIN32__`, silent g++ failures, `-static` linking).
4. **`04-display-pipeline.md`** — From 8bpp palette indices to the ST7789 panel: the
   BGR555-to-byteswapped-RGB565 LUT, 48-line bands sized to the S3's 32 KB DMA cap,
   ping-pong buffers, and the measured 16.37 ms flush. Why LovyanGFX was rejected.
5. **`05-memory-map.md`** — The make-it-fit story: what stays in internal SRAM (hot),
   what moves to PSRAM via `EWRAM_COLD` (cold), why the framebuffer must never live in
   PSRAM, and the numbers behind every placement.
6. **`06-engine-port.md`** — The actual engine work: the shared MODE13 `rasterizer.h`
   (three upstream DOS bugs fixed), the `render.iwram.cpp` copy trick, the 32-bit-pointer
   constraint, PKD level loading and why levels reload pristine from flash each start.
7. **`07-build-system.md`** — The ESP-IDF firmware build, driven by cmake+ninja without
   `idf.py`: `build.ps1`/`flash.ps1`, the sdkconfig flags that matter, `partitions.csv`,
   the `OLVL` level container and `make_levels.py`. (The simulator's Makefile is covered
   in `03-windows-simulator.md`.)
8. **`08-performance.md`** — Measured numbers, where the frame time goes, and the
   not-yet-pulled levers (IRAM rasterizer, second core, relaxing the GBA potato profile).
9. **`09-research-notes.md`** — The verified research that drove the design: PSRAM vs
   flash bandwidth (with sources), the 64-byte cache line requirement, and why a second
   ESP32 as GPU was evaluated and rejected.
10. **`10-roadmap.md`** — Current status (phases 1-4a done) and what remains: audio
    output (I2S vs PWM), performance phase, 320x240 title screen asset, saves, SD levels.

## Repository layout

Everything the port added lives under `src/platform/esp32/` (plus a small
`#elif defined(__ESP32__)` ladder in `src/fixed/common.h` and friends — see
`06-engine-port.md`):

```
src/platform/esp32/
├── rasterizer.h            # shared MODE13 (8bpp) rasterizer — single source of truth,
│                           #   based on dos/rasterizer.h with 3 upstream bugs fixed
├── win/                    # Windows simulator (__ESP32_WIN__)
│   ├── Makefile            # i686-only build, refuses x86_64 compilers
│   ├── main.cpp            # Win32 harness: window, blit, waveOut, input, 33ms tick
│   ├── rasterizer.h        # forwarder -> ../rasterizer.h
│   ├── render.cpp          # generated: copy of gba/render.iwram.cpp (make rule)
│   └── data/               # TITLE/GYM/LEVEL1/LEVEL2.PKD + TRACKS.AD4 (copied from gba/data)
├── idf/                    # ESP-IDF v5.5 firmware project
│   ├── build.ps1           # cmake+ninja build, no idf.py; full env recipe inside
│   ├── flash.ps1           # esptool flash; -Levels also builds+flashes level data
│   ├── make_levels.py      # packs PKDs + TRACKS.AD4 into the OLVL container
│   ├── partitions.csv      # nvs / phy / factory 3MB / levels 12.9MB @ 0x310000
│   ├── sdkconfig.defaults  # octal PSRAM 80MHz, 64KB/64B/8-way D-cache, QIO 80MHz...
│   ├── CMakeLists.txt
│   └── main/
│       ├── game_main.cpp   # firmware harness: boot, level mount, input, game loop
│       ├── display.h       # 3-function display interface
│       ├── display_esplcd.c# ST7789 esp_lcd DMA pipeline (measured 16.37 ms/frame)
│       ├── board_pins.h    # all board + button pin definitions
│       ├── main.c          # phase-2 display benchmark (kept, not compiled)
│       ├── rasterizer.h    # forwarder -> ../../rasterizer.h
│       ├── render.cpp      # generated at configure time (CMake configure_file)
│       └── CMakeLists.txt
└── doc/                    # this documentation set
```

The `render.cpp` files are *generated copies* of `src/platform/gba/render.iwram.cpp`,
never edited by hand: the generic renderer does `#include "rasterizer.h"` with quotes,
which resolves relative to the including file's directory, so each platform places a
copy of the renderer next to its own rasterizer (here, a one-line forwarder). This is
the same trick upstream's `dos/deploy.bat` and `tns/Makefile` use — see
`06-engine-port.md`.

## Credits

- **OpenLara** — engine and all game-logic code by **XProger**:
  [github.com/XProger/OpenLara](https://github.com/XProger/OpenLara). The fixed-point
  `src/fixed/` engine and the GBA renderer this port builds on are his work.
- **ESP32-S3 port** — branch `esp32s3` of the fork
  [github.com/SoftwareEscarlata/OpenLara](https://github.com/SoftwareEscarlata/OpenLara).
  Key commits: `b278f9e` (Windows simulator), `0f3db80` (display pipeline, measured),
  `0d3816f` (engine running on hardware).
- Tomb Raider is a trademark of its respective owners; you must provide your own level
  data (the `.PKD` files are converted from the original game assets).

## Key takeaways

- Full Tomb Raider 1 runs on a $20 ESP32-S3 board at 48-63 fps — faster than the
  original 30 fps game — by porting OpenLara's fixed-point GBA engine, not the desktop
  float/OpenGL engine.
- The port is conceptually a memory re-mapping: GBA cart ROM -> flash mmap, EWRAM ->
  octal PSRAM, IWRAM -> internal SRAM; almost all engine code is untouched.
- Three ways to run it: byte-exact Windows simulator (`win/`, i686 g++ + `mingw32-make`),
  firmware build/flash (`idf/build.ps1` + `flash.ps1 -Levels`), and play over USB serial
  with `miniterm` or physical buttons on header P1.
- The display is the frame-rate ceiling: one full 320x240 RGB565 frame costs 16.37 ms
  over 80 MHz SPI, hidden behind DMA ping-pong buffers.
- Start with `01-architecture.md` for the why; each numbered doc covers one layer of
  the port in depth.
