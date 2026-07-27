# The Windows Simulator: a Byte-Exact Reference Build

This document explains the Windows simulator that lives in `src/platform/esp32/win/` —
what it is, why it exists, why it *must* be a 32-bit executable, how every subsystem of
its harness works (blit, palette, timing, input, sound), and every toolchain trap that
was hit while bringing it up, each with its symptom, cause, and fix. By the end you
should be able to build it, play Tomb Raider 1 in a window, and understand exactly which
parts of what you are seeing will behave identically on the ESP32-S3.

For the firmware-side build that consumes the same engine code, see
`07-build-system.md`. For where the engine's data lives on the device, see
`05-memory-map.md`.

## 1. Why a PC reference build eliminates most porting risk

Porting a 100k-line engine to a microcontroller couples two very different problems:

1. **Is the engine configured and glued correctly?** (defines, rasterizer, level
   loading, palette handling, input mapping, memory sizes)
2. **Does it run on this specific silicon?** (toolchain, RAM layout, cache, DMA,
   display timing)

If you debug both at once, every logic bug costs you a 2-minute flash cycle and gives
you a panic dump on a serial port as your only debugger. The fix — proven previously on
the `esp32s3-arcade-3d` project and reused here deliberately — is to split them: build
the *exact same engine configuration* as a native PC executable first, and only move to
hardware once the picture on screen is correct.

| Environment | Answers | Iteration cycle | Fidelity |
|---|---|---|---|
| Windows simulator | "is the logic right? do the pixels look right?" | ~5 s | Logic and pixels: 100%. Timing: 0% |
| Real hardware | "how many FPS? does it fit in RAM?" | ~2 min | The only truth about performance |

The working rule during the port was: **~90% of all iterations happen in the
simulator**. Hardware is only for measuring. This paid off exactly as designed: three
upstream rasterizer bugs (an undefined `VRAM_WIDTH`, a missing `R = L;` edge reset that
crashes on flat-shaded faces, and four empty TODO stub functions) were all found and
fixed in a window on a PC, in seconds per attempt — and when the engine finally ran on
the ESP32-S3, the *only* engine-code change the Xtensa toolchain required was a single
`#include <new>`. Everything else "just worked" because it had already worked, byte for
byte, in this simulator.

The simulator runs the full game at **64 FPS** on the development PC and has run for
hours without a crash.

## 2. What "byte-exact" means here

The simulator is not a look-alike. It is the same engine, with the same compile-time
profile as the device target. `src/fixed/common.h` defines the two platforms side by
side:

```cpp
#if defined(__ESP32_WIN__)
    // ESP32-S3 Windows simulator: byte-exact reference for the ESP32-S3 port.
    // Same MODE13/320x240/PKD profile the real __ESP32__ target will use.
    #define USE_DIV_TABLE

    #define MODE13
    #define FRAME_WIDTH  320
    #define FRAME_HEIGHT 240
    // fb row stride in uint16 units (2 pixels per element)
    #define VRAM_WIDTH   (FRAME_WIDTH / 2)

    #define USE_FMT     (LVL_FMT_PKD)

    #define _CRT_SECURE_NO_WARNINGS
    #include <windows.h>
#elif defined(__ESP32__)
    // ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-2): dual LX7 @240MHz, 512KB SRAM,
    // 8MB octal PSRAM. Levels are copied from mmap'd flash into PSRAM at load.
    #define USE_DIV_TABLE

    #define MODE13
    #define FRAME_WIDTH  320
    ...
```

Same renderer mode (`MODE13`, 8bpp paletted), same resolution, same fixed-point math
(`FIXED_SHIFT 14`), same level format (`.PKD`), same division-by-table optimization.
Both targets also share:

- **The same rasterizer.** `src/platform/esp32/rasterizer.h` is the single MODE13
  rasterizer used by both builds. The simulator's `src/platform/esp32/win/rasterizer.h`
  is a 4-line forwarder:

  ```cpp
  // Forwarder: render.cpp (copy of gba/render.iwram.cpp) does #include "rasterizer.h"
  // which resolves to the directory of the including file — this file forwards to the
  // shared ESP32 MODE13 rasterizer used by both the Windows sim and the real target.
  #include "../rasterizer.h"
  ```

- **The same generic renderer.** `src/platform/gba/render.iwram.cpp` (~1200 lines) is the
  software renderer shared by *all* "potato" platforms. Its line 61 does
  `#include "rasterizer.h"` with quotes — quoted includes resolve relative to the
  *including file's* directory — so every platform copies `render.iwram.cpp` next to its
  own `rasterizer.h`. The DOS port does this in `deploy.bat`, the TI-Nspire port in its
  Makefile, and both our builds do it too (the simulator via a Makefile rule, the
  firmware via CMake `configure_file`).

- **The same chord-based input scheme** (`src/fixed/lara.h:2748` puts `__ESP32__` and
  `__ESP32_WIN__` in the same branch as `__GBA__`), the same sound mixer
  (`src/platform/gba/sound.cpp`), and the same unmodified GBA `.PKD` level files.

The only code that differs is the OS layer: `src/platform/esp32/win/main.cpp` (~350
lines) versus `src/platform/esp32/idf/main/game_main.cpp`. The firmware harness was
written by *transcribing* the Windows one — same loop shape, same `os*` contract.

One deliberate model choice is worth recording: the harness is modeled on the
`__GBA_WIN__` block inside `src/platform/gba/main.cpp`, **not** on
`src/platform/win_fixed/` (that is the desktop GL/PHD build — a different engine
configuration entirely) and not on `src/platform/tns/main.cpp` (stale API, bitrotted
against the current engine in three separate ways). When cloning a reference, pick the
one that actually compiles against today's engine.

One accepted difference: on `__ESP32_WIN__` the framebuffer keeps the upstream
oversized declaration `uint16 fb[FRAME_WIDTH * FRAME_HEIGHT]` (153.6 KB — a GBA VRAM
legacy, 2x what 8bpp needs), while `__ESP32__` right-sizes it to
`uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2]` (76.8 KB). See `common.h:402-407`. The sim
keeps the fat version purely to minimize the diff against upstream; only the first
`W*H` bytes are ever used on either target, so the rendered bytes are identical.

## 3. Why it must be 32-bit

This is not a preference; a 64-bit build of this engine cannot work. The fixed-point
engine stores **pointers in `uint32` fields** in at least two load-bearing places:

**1. Level offset fixups.** `.PKD` files store file-relative offsets which are patched
into absolute pointers at load by adding the load address — as a `uint32`
(`src/fixed/fmt/pkd.h:14-18` and `:71-80`):

```cpp
uint32* ptr = (uint32*)&level.palette;
while (ptr <= (uint32*)&level.soundOffsets)
{
    *ptr++ += (uint32)data;
}
...
level.textures[i].tile += (uint32)level.tiles;
```

On a 64-bit build, truncating a pointer to 32 bits and storing it back into a structure
field is at best a compile error and at worst silent corruption.

**2. Color smuggling in the rasterizer.** For flat-shaded faces, the renderer passes a
color index (0..255) *cast to a `VertexLink*`* through the right-edge parameter of
`rasterizeF`. The rasterizer extracts the low bits as the color and must then reset the
pointer (`R = L;`) before edge-walking. This trick assumes pointer and `uint32` are
interchangeable.

The ESP32-S3's Xtensa LX7 is a 32-bit core, so none of this is a problem on the device
— which is precisely why the *reference* build must also be 32-bit. An x86_64 MinGW
compiler was tried and fails on these casts (verified, not theoretical). The Makefile
therefore hard-refuses 64-bit compilers:

```makefile
# HARD REQUIREMENT: 32-bit (i686) g++. The fixed engine stores pointers in
# uint32 (fmt/pkd.h offset fixups, rasterizer color smuggling) — 64-bit builds
# fail to compile and would corrupt at runtime.
CXX = C:/msys64/mingw32/bin/g++.exe

# refuse to build 64-bit — catches accidental use of a x86_64 g++
ifneq (,$(findstring x86_64,$(shell $(CXX) -dumpmachine)))
$(error $(CXX) targets x86_64; the fixed engine needs 32-bit pointers — use an i686 g++)
endif
```

Note the nuance in the *harness* code, though: the harness itself is written x64-safely
where it can be (e.g. `SetWindowLongPtr`/`LONG_PTR` instead of the legacy
`SetWindowLong`) so that the 32-bit constraint stays confined to the engine, not
baked into new code.

## 4. How the harness works

All code below is from `src/platform/esp32/win/main.cpp`.

### 4.1 Window and blit path

The window is created with the stock `"static"` window class and then subclassed —
no `RegisterClass` boilerplate, same trick as the GBA sim:

```cpp
hWnd = CreateWindow("static", "OpenLara ESP32-S3 sim", WS_OVERLAPPEDWINDOW, ...);
hDC = GetDC(hWnd);
SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)&wndProc);
```

The window is `WND_SCALE 3`x the framebuffer: 960x720 for a 320x240 game.

Each frame, `blit()` expands the 8bpp indexed framebuffer through the palette into a
32-bit staging buffer and pushes it to the window with `StretchDIBits`:

```cpp
void blit()
{
    // fb is 8bpp indexed (first W*H bytes of the uint16 array) — expand
    // through the BGR555 palette. This models exactly what the ESP32-S3
    // will do per DMA chunk (palette -> RGB565 instead of RGB888).
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
    {
        uint16 c = MEM_PAL_BG[((uint8*)fb)[i]];
        SCREEN[i] = (((c << 3) & 0xFF) << 16) | ((((c >> 5) << 3) & 0xFF) << 8)
                  | ((c >> 10 << 3) & 0xFF) | 0xFF000000;
    }
    const BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), FRAME_WIDTH, -FRAME_HEIGHT,
                               1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    StretchDIBits(hDC, 0, 0, WND_WIDTH, WND_HEIGHT, 0, 0, FRAME_WIDTH, FRAME_HEIGHT,
                  SCREEN, &bmi, DIB_RGB_COLORS, SRCCOPY);
}
```

Three details matter:

- **The palette is BGR555 with red in the low bits** (GBA-style). Bit layout of each
  `uint16` entry:

  | Bits | 14..10 | 9..5 | 4..0 |
  |---|---|---|---|
  | Channel | Blue | Green | Red |

  The expansion shifts each 5-bit channel up by 3 into an `0xAARRGGBB` pixel: red
  (`(c << 3) & 0xFF`) into bits 16-23, green into 8-15, blue into 0-7.

- **The negative height in `BITMAPINFO` (`-FRAME_HEIGHT`) declares a top-down DIB.**
  Windows bitmaps are bottom-up by default; the negative height makes row 0 the top
  row, matching how the engine writes the framebuffer — no vertical flip pass needed.

- **This loop is a model of the device pipeline.** On the ESP32-S3 the same
  index-through-LUT expansion happens per 48-line DMA band, with the LUT holding
  pre-byteswapped RGB565 instead of XRGB8888 (see
  `src/platform/esp32/idf/main/display_esplcd.c`). If a palette or indexing bug exists,
  it shows up here first, on identical input bytes.

### 4.2 Palette flow

The engine never draws in RGB. The full flow is:

1. The level loader (`src/fixed/fmt/pkd.h:42`) and the fade logic
   (`updateFading` -> `src/fixed/level.h:257`) call
   `palSet(level.palette, gamma, brightness)` — every fade tick during fade-in/out.
2. `palSet` (`src/fixed/common.cpp:1549`) applies gamma/brightness to the 256 BGR555
   entries. When it needs scratch space for the adjusted copy, it borrows the
   `gSpheres` array as temporary memory rather than allocating (a very GBA move —
   there is even a commented `STATIC_ASSERT(sizeof(gSpheres) >= 512)` next to it).
3. `palSet` hands the final palette to the platform via `osSetPalette`.
4. The simulator's `osSetPalette` just snapshots it into a shadow copy:

   ```cpp
   void osSetPalette(const uint16* palette)
   {
       memcpy(MEM_PAL_BG, palette, 256 * 2);
   }
   ```

5. The expansion to displayable pixels happens **at blit time**, from the shadow copy.

On the device the same step 4 becomes `displaySetPalette`, which rebuilds the 256-entry
RGB565 LUT. Identical contract, identical call frequency (once per fade tick, not per
pixel).

### 4.3 Timing model: 33 ms ticks

The engine's `gameUpdate(frames)` argument is a count of **33 ms ticks** — the game
logic runs at 30 Hz regardless of render rate, exactly like the original game. The main
loop derives the tick count from wall time and renders as fast as it can:

```cpp
int32 startTime = GetTickCount() - 33;
int32 lastFrame = 0;
...
int32 frame = (GetTickCount() - startTime) / 33;
if (GetAsyncKeyState('R')) frame /= 10;   // slow-mo

int32 count = frame - lastFrame;
if (GetAsyncKeyState('T')) count *= 10;   // fast-forward
gameUpdate(count);
lastFrame = frame;

gameRender();
blit();
```

Because logic time is decoupled from render time, the two debug keys are nearly free:

- **T** — fast-forward: multiplies the tick delta by 10. Useful to skip to a specific
  game situation in seconds.
- **R** — slow-motion: divides elapsed ticks by 10. Useful to inspect a single
  animation or rasterizer artifact frame by frame.

The loop `Sleep(1)`s per iteration so it does not busy-spin a core, and counts rendered
frames per wall second into `fps` (the value the in-game FPS counter displays; 64 on
the dev machine). The firmware loop in `game_main.cpp` is the same structure with
`esp_timer_get_time()/1000` in place of `GetTickCount()` and `displayFlush(fb)` in
place of `blit()`.

### 4.4 Input mapping and the GBA chord scheme

The engine's input word is a GBA-style bitmask (`IK_UP/DOWN/LEFT/RIGHT/A/B/L/R/
START/SELECT`). The window proc maps keyboard to `IK_*` on `WM_KEYDOWN`/`WM_KEYUP`:

| PC key | Engine bit | Role |
|---|---|---|
| Arrow keys | `IK_UP/DOWN/LEFT/RIGHT` | movement / menu navigation |
| S | `IK_A` | GBA A |
| A | `IK_B` | GBA B |
| Q | `IK_L` | GBA L (chord modifier) |
| W | `IK_R` | GBA R |
| Enter | `IK_START` | inventory / confirm |
| Space | `IK_SELECT` | select |

Ten buttons control a game designed for a full keyboard because the engine uses the
GBA **chord scheme** (`src/fixed/lara.h:2748-2788`, shared by `__GBA__`, `__GBA_WIN__`,
`__ESP32__` and `__ESP32_WIN__`):

| Chord | In-game action |
|---|---|
| A | action (grab, shoot, pull switch) |
| A + L | draw / switch weapon |
| B | jump |
| B + L | roll |
| R | walk (hold) |
| R + L | look |

(`gSettings.controls_swap` can swap the A/B roles.) So in the simulator: **S** =
action, **A** = jump, hold **W** to walk, **Q+S** to draw weapons, **Q+A** to roll.

Number keys 1-4 are debug weapon cheats (pistols/magnums/uzis/shotgun). Upstream's GBA
sim crashes if you press them on the title screen — `players[0]` is NULL until a level
spawns Lara — so the port adds a guard:

```cpp
if (players[0] && players[0]->extraL) // NULL on title screen
{
    if (wParam == '1') players[0]->extraL->goalWeapon = WEAPON_PISTOLS;
    ...
```

`WM_ACTIVATE` clears `keys` so alt-tabbing away doesn't leave a phantom held key.

### 4.5 Sound: waveOut double buffering

The simulator reuses the GBA mixer (`src/platform/gba/sound.cpp`) unmodified — pure C
path, since `USE_ASM` is not defined — and plays its output with the ancient but
dependable `waveOut` API: 8-bit unsigned mono at 22050 Hz
(`SND_SAMPLES 1024`, `SND_OUTPUT_FREQ 22050` from `common.h:456-462`):

```cpp
WAVEFORMATEX waveFmt = { WAVE_FORMAT_PCM, 1, SND_OUTPUT_FREQ, SND_OUTPUT_FREQ,
                         1, 8, sizeof(waveFmt) };
```

Two `WAVEHDR`s point into the mixer's own `soundBuffer` (declared in
`gba/sound.cpp`), and the refill is event-driven: when a buffer finishes playing,
Windows posts `MM_WOM_DONE` to the window, and the handler calls `soundFill()`, which
unprepares the finished header, runs `sndFill()` (the engine mixer) into it, and
re-queues it. Two 1024-sample buffers at 22050 Hz means ~46 ms of latency per buffer —
comfortably inaudible for this kind of game, and structurally identical to the
ping-pong DMA plan for the device's I2S output (Phase 4b).

### 4.6 Data loading and the RAM budget instrumentation

`osLoadLevel` reads `data/<NAME>.PKD` from disk into a fresh heap block each time a
level (re)starts. This mirrors a hard requirement discovered during the port: **the
level image is mutated in place** — pointer fixups at load, animated-texture shifts
every 5 frames, camera flags at runtime — so a pristine copy is needed per level start.
On the device that is a `memcpy` from flash mmap into PSRAM; here it is a file re-read.

Two graceful fallbacks keep the sim usable with partial data:

- `data/TRACKS.AD4` missing -> a 512-byte zeroed table is substituted, which makes
  every music track have size 0 and be skipped (game runs, silent music).
- `data/TITLE.SCR` is validated by size: `renderBackground` copies exactly
  `FRAME_WIDTH * FRAME_HEIGHT` bytes, and the GBA asset is 240x160 = 38400 bytes, not
  76800 — so a wrong-size file is rejected and a zeroed (black) background is used
  until a 320x240 asset is regenerated with the GBA packer. The firmware does the
  same thing.

Finally, the sim doubles as a memory watchdog. The real board has ~440 KB of usable
internal SRAM and 8 MB of PSRAM; the sim can't model the internal SRAM layout, but it
tracks every heap allocation the platform layer makes and prints a report against the
PSRAM budget on each level load:

```cpp
#define ESP32_PSRAM_BUDGET (8 * 1024 * 1024)
...
LOG("RAM: level %u KB + tracks %u KB + title %u KB = %u KB heap (PSRAM budget %u KB)\n", ...);
```

Overruns show up in a console line on the PC, not as a `malloc` returning NULL after a
2-minute flash cycle. (The internal-SRAM side of the story — `EWRAM_COLD`, the
right-sized `fb`, what stays hot — is covered in `05-memory-map.md`.)

## 5. Toolchain gotchas — symptom, cause, fix

Every one of these is real and cost time during bring-up. They are listed in the order
you are likely to hit them.

**1. Engine won't compile: pointer-cast errors everywhere.**
- *Symptom:* dozens of errors like "cast from pointer to `uint32` loses precision" in
  `pkd.h` and the rasterizer.
- *Cause:* you are using an x86_64 g++. The engine stores pointers in `uint32`
  (section 3); 64-bit builds are impossible by design.
- *Fix:* install the 32-bit compiler: `pacman -S mingw-w64-i686-gcc` in MSYS2, giving
  `C:/msys64/mingw32/bin/g++.exe`. The Makefile's `-dumpmachine` guard turns this
  mistake into one clear error message instead of a wall of casts.

**2. Wrong platform branch: the build wants OpenGL / desktop headers.**
- *Symptom:* `common.h` selects the desktop GL1/PHD configuration; errors reference
  floats, GL types, or `LVL_FMT_PHD` paths that the potato engine should never touch.
- *Cause:* MinGW g++ **predefines `__WIN32__`**, and `common.h`'s platform ladder
  checks `__WIN32__` before your `-D__ESP32_WIN__` ever matters.
- *Fix:* compile with `-U__WIN32__` (see `DEFS` in the Makefile). This is mandatory,
  not cosmetic.

**3. g++ "fails" with exit code 1 and zero output.**
- *Symptom:* make reports an error; the compiler prints *nothing at all*. No
  diagnostics, no ICE, nothing.
- *Cause:* `g++.exe` was invoked by absolute path but `C:/msys64/mingw32/bin` is not
  on `PATH`, so Windows cannot load the compiler's own runtime DLLs. The process dies
  before `main`, and all you see is the exit code.
- *Fix:* put `C:\msys64\mingw32\bin` on `PATH` (running from the MSYS2 MinGW32 shell
  does this for you). Remember this failure signature — silent exit 1 on Windows
  almost always means DLL resolution, not compilation.

**4. The built exe crashes instantly on an unrelated machine/shell.**
- *Symptom:* `OpenLara_esp32sim.exe` dies at startup with no window.
- *Cause:* dynamic linking picked up a **mismatched `libwinpthread-1.dll`** (or
  similar) from somewhere else on the system `PATH` — a classic MinGW deployment trap.
- *Fix:* link with `-static` (see `LDFLAGS`). The exe becomes self-contained and runs
  anywhere.

**5. The Makefile's compiler setting is silently ignored.**
- *Symptom:* you set `CXX ?= C:/msys64/mingw32/bin/g++.exe` but the build uses plain
  `g++` (typically the 64-bit one) anyway.
- *Cause:* GNU make has a **builtin default** for `CXX` (`g++`), so the variable is
  never "undefined" and `?=` never assigns.
- *Fix:* use a plain `CXX =` assignment.

**6. Release (`NDEBUG`) builds break on the logging macro.**
- *Symptom:* g++ errors like "macro `LOG` passed N arguments, but takes just 0" the
  moment `_DEBUG` is off.
- *Cause:* upstream defined the no-op variant as a zero-argument macro (`#define
  LOG()`), but call sites pass arguments. MSVC tolerates this; g++ does not.
- *Fix:* make it variadic — `#define LOG(...)` (`src/fixed/common.h:212`).

**7. Nothing stops the *next* person from hitting #1.**
- *Symptom:* n/a — this one is preventive.
- *Cause:* toolchain requirements documented only in a README get ignored.
- *Fix:* the Makefile guard shown in section 3: query `$(CXX) -dumpmachine` and
  `$(error ...)` out if it contains `x86_64`. Encode hard requirements in the build,
  not in prose.

## 6. Building and running

Prerequisites (once):

```
# in an MSYS2 shell
pacman -S mingw-w64-i686-gcc make
```

Build:

```
cd src/platform/esp32/win
mingw32-make            # or `make` — run from an MSYS2/Git Bash environment:
                        # the data rule uses `cp` and `mkdir -p`
```

The default target builds `OpenLara_esp32sim.exe` **and** populates `data/` by copying
the game data straight out of the GBA port's tree — `TITLE.PKD`, `GYM.PKD`,
`LEVEL1.PKD`, `LEVEL2.PKD`, `TRACKS.AD4` from `src/platform/gba/data/`. These are the
same unmodified files the firmware packs into its flash partition (see
`07-build-system.md`); little-endian 32-bit layout loads as-is on both x86 Win32 and
Xtensa.

Note the `render.cpp` rule: it *copies* `src/platform/gba/render.iwram.cpp` into the
local directory before compiling it, so the quoted `#include "rasterizer.h"` resolves
to the local forwarder (section 2). `mingw32-make clean` removes the copy along with
objects and the exe.

Run `OpenLara_esp32sim.exe` from the directory containing `data/`. Controls:

| Key | Function |
|---|---|
| Arrows | move / menus |
| S | A — action; +Q = draw/switch weapon |
| A | B — jump; +Q = roll |
| Q | L — chord modifier |
| W | R — walk; +Q = look |
| Enter | START — inventory / confirm |
| Space | SELECT |
| 1 / 2 / 3 / 4 | weapon cheat: pistols / magnums / uzis / shotgun (in-level only) |
| T (hold) | 10x fast-forward |
| R (hold) | 10x slow-motion |

Settings and saves persist to `settings.dat` / `savegame.dat` next to the exe.

## Key takeaways

- A native reference build with the target's exact compile profile converts panic dumps
  into 5-second edit-compile-look loops; ~90% of all porting iterations happened here.
  The precedent (`esp32s3-arcade-3d`) held: after the sim was correct, the engine
  needed exactly one code change (`#include <new>`) to run on Xtensa.
- Byte-exactness is structural, not aspirational: same `common.h` defines, the same
  shared `rasterizer.h` (via a forwarder), the same copied `render.iwram.cpp`, the
  same PKD files. Only the ~350-line OS harness differs.
- The engine is 32-bit-pointer-only (`*ptr += (uint32)data` in `pkd.h`, color smuggled
  as a `uint32` cast to `VertexLink*` in the rasterizer). The simulator must be built
  with i686 g++, and the Makefile enforces this rather than documenting it.
- The blit path is a faithful model of the device's display pipeline: 8bpp indices
  expanded through a BGR555 shadow palette at flush time — XRGB8888+`StretchDIBits`
  on PC, pre-byteswapped RGB565 LUT + DMA bands on the S3.
- Game logic is 30 Hz (33 ms ticks) decoupled from render rate, which makes the T/R
  fast-forward/slow-motion debug keys nearly free.
- Windows toolchain failures are frequently *silent* (exit 1 with no output = DLL
  load failure; `?=` silently losing to make's builtin `CXX`; MinGW predefining
  `__WIN32__`). Encode every hard requirement — 32-bit compiler, `-U__WIN32__`,
  `-static` — into the Makefile itself.
