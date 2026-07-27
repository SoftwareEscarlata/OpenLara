# 01 — The Two Engines of OpenLara and Why We Ported the Potato One

This document explains the single most important discovery of the port: OpenLara is not one
engine but two, and only one of them can run on a microcontroller. You will learn how the
`src/` and `src/fixed/` trees differ, why fixed-point 8bpp software rendering is the only
viable MCU path, how the Game Boy Advance memory model maps almost one-to-one onto the
ESP32-S3, how the "generic renderer + per-platform rasterizer" architecture works (including
the quoted-include copy trick that makes it possible), what translation units a potato build
actually consists of, and which `#elif` ladders in `src/fixed/common.h` every new platform
must join. Everything here is grounded in the actual repository code; file references are
relative to the repo root.

## 1. Two engines in one repository

Browsing OpenLara from the top, you first find `src/`: `core.h`, GLSL shaders, `mat4`/`vec4`
float math, D3D/GL/GLES/Vulkan backends. That engine targets Windows, Linux, Android, PSP,
3DS, Switch — machines with megabytes of RAM and a GPU. Porting *that* to an ESP32-S3 is
hopeless.

But there is a second, complete, independent engine in `src/fixed/`, written by XProger to
run full Tomb Raider 1 on a Game Boy Advance — an ARM7TDMI at 16.78 MHz with 288 KB of RAM
and no floating-point unit, no divider, no GPU. Internally we called it the **potato
engine**, and it is the one we ported.

| | `src/` (main engine) | `src/fixed/` (potato engine) |
|---|---|---|
| Math | float, `mat4`, `vec4` | pure integer, `FIXED_SHIFT 14` (`src/fixed/common.h:32`) |
| Rendering | OpenGL / GLES / D3D8 / Vulkan, shaders | software rasterizer, 8bpp paletted |
| Color | true color | 256-entry palette, `gLightmap[256*32]` shading |
| RAM footprint | tens of MB | fits a GBA: 288 KB total |
| Level format | PHD/PSX/TR2/TR4... | PKD (repacked, load-by-cast, little-endian 32-bit) |
| Platforms | Win, Linux, Android, PSP, 3DS, Switch... | GBA, Sega 32X, 3DO, DOS, TI-Nspire |
| Pointer width | 64-bit clean | **32-bit only** (pointers stored in `uint32`) |

The two engines share nothing but the repository. Game logic, collision, animation, AI,
inventory, sound mixing — everything exists twice. `src/fixed/` is not a "reduced" build of
`src/`; it is a parallel implementation designed from the start for machines like our target.

One consequence of its GBA heritage matters enough to state up front: the potato engine is
**32-bit-pointer-only**. The PKD loader patches file offsets into live pointers by integer
addition (`src/fixed/fmt/pkd.h:17`):

```cpp
*ptr++ += (uint32)data;
```

and the rasterizer smuggles a color index through a `VertexLink*` parameter (see
`06-engine-port.md`). A 64-bit build cannot even compile these casts. The ESP32-S3's Xtensa
LX7 is 32-bit little-endian, exactly like the GBA's ARM7TDMI — which is also why the GBA's
`.PKD` level files load byte-for-byte unmodified on both x86 Win32 and Xtensa.

## 2. Why fixed-point 8bpp is the only viable MCU path

Three independent constraints each rule out the main engine on their own; the potato engine
sidesteps all three by design.

**No GPU, no GL.** The main engine renders through GAPI backends (`#define GAPI_GL1` in the
`__WIN32__` branch of `src/fixed/common.h` is the *desktop* potato variant; `src/` proper
uses shaders). The ESP32-S3 has no display controller beyond SPI DMA — every pixel must be
produced by the CPU. The potato engine's renderer *is* a CPU rasterizer already: affine
texture spans, painter's-order sorting via an ordering table (`gOT`), palette-lookup
lighting. There is nothing to replace, only somewhere new to send the finished framebuffer.

**RAM.** A 320x240 true-color framebuffer alone is 300 KB (32bpp) or 150 KB (RGB565) —
before level data, before engine state — against ~440 KB of usable internal SRAM. The
potato engine's `MODE13` profile uses an 8bpp indexed framebuffer:

```
320 x 240 x 1 byte = 76,800 B   8bpp indexed  <- fits internal SRAM comfortably
320 x 240 x 2 byte = 153,600 B  RGB565        <- doubles RAM *and* memory bandwidth
```

The 8bpp choice is load-bearing beyond size: the rasterizer's lighting model *is* the
palette. Every textured pixel goes through `gLightmap[256*32]` (8 KB, hot in cache), which
maps (shade, color index) to a final palette index. Converting the engine to RGB565 would
mean rewriting every span loop and losing the lightmap. Instead, palette-to-RGB565 expansion
happens once per frame, per DMA band, during the display flush (see
`04-display-pipeline.md`).

**No FPU, slow division.** All potato math is integer with a fixed-point shift of 14, and
`USE_DIV_TABLE` replaces divisions with a reciprocal table (`divTable`, 2 KB, used per span).
The LX7 does have an FPU, but the engine does not need us to care — and the proven integer
path is faster than a naive float port would be anyway.

The clincher is empirical: this engine ships on a 16.78 MHz ARM7TDMI. The ESP32-S3 runs two
LX7 cores at 240 MHz — roughly 14x the clock of the GBA on one core, with a second core to
spare and ~1.8x the internal SRAM. Measured result on the board: title screen at 62-63 fps
(display-limited) and in-game GYM at 48-50 fps, versus the original game's 30 fps.

## 3. The GBA-to-ESP32-S3 conceptual mapping

The port's memory design is a translation of the GBA's three-tier memory model onto the
ESP32-S3's three tiers. Once you see this mapping, every placement decision in the port
follows from it.

| GBA tier | What the GBA uses it for | ESP32-S3 equivalent | Mechanism in the port |
|---|---|---|---|
| Cart ROM (up to 32 MB, read-only) | level data, music, code | 16 MB flash, memory-mapped | `esp_partition_mmap()` of the `levels` partition (OLVL container) |
| EWRAM (256 KB, slow 16-bit bus) | big, rarely-touched arrays | 8 MB octal PSRAM (cache-backed) | new `EWRAM_COLD` macro = `EXT_RAM_BSS_ATTR` (`src/fixed/common.h:388`) |
| IWRAM (32 KB, fast 32-bit) + VRAM | hot state, framebuffer | ~440 KB internal SRAM | default `.bss`/`.data` placement |

The engine already encodes hot/cold placement in its source: GBA builds tag large cold
arrays `EWRAM_DATA` and leave hot state in IWRAM. The port introduces one new macro,
`EWRAM_COLD`, to split the *coldest* subset of `EWRAM_DATA` out to PSRAM:

```cpp
// "cold EWRAM": large, rarely-touched arrays that the GBA already parks in its
// slow EWRAM. On ESP32-S3 they live in octal PSRAM (cache-backed) so internal
// SRAM stays free for the framebuffer and hot engine state.
#if defined(__ESP32__)
    #define EWRAM_COLD EXT_RAM_BSS_ATTR
#else
    #define EWRAM_COLD EWRAM_DATA
#endif
```

On every other platform `EWRAM_COLD` degrades to `EWRAM_DATA`, so the annotation costs
upstream nothing. It is applied to exactly four arrays — `gBackgroundCopy` (75 KB,
inventory-background only), `gSaveData` (8 KB), `dynSectors` (24 KB), `enemiesExtra`
(~25 KB at `MAX_ENEMIES 3`) — freeing ~130 KB of internal SRAM. Before it existed, the
engine's `.dram0.bss` overflowed the S3's `dram0_0_seg` by 2528 bytes at link time; after
it, 33 KB of internal heap remain free with the full game running. Hot data — `fb`
(76.8 KB), `gVertices` (40 KB), `gFaces` (30 KB), `divTable`, `items`, `gLightmap` — stays
internal. The full inventory is in `05-memory-map.md`.

Two refinements go beyond the literal mapping:

- **The active level is copied flash -> PSRAM at load** rather than read from mmap'd flash
  like a GBA cart. Octal PSRAM serves a random cache miss 3-4x faster than quad flash, and
  flash and PSRAM share the SPI0 bus and D-cache — texture reads from flash would also
  contend with instruction fetch. The copy costs 16 ms for TITLE.PKD (306 KB). Sources and
  numbers in `09-research-notes.md`.
- **`ROM_READ` is not defined.** On GBA that flag copies four tables out of read-only
  cart data into RAM statics. Our level copy in PSRAM is writable, so the engine can patch
  it in place — which it does, at load and at runtime (see `06-engine-port.md` for why that
  forces a pristine re-copy per level start).
- **The soundtrack (`TRACKS.AD4`, 3 MB) stays flash-mmap'd**, because the mixer reads it
  sequentially and sequential flash-through-cache is cheap; only random access is expensive.

## 4. The generic renderer and the per-platform rasterizer

The potato render stack is split in two layers, and the way they bind together is the
most unusual build trick in the repository.

**Layer 1 — the generic renderer:** `src/platform/gba/render.iwram.cpp` (1199 lines
upstream, 1201 on this branch). Despite living in the GBA directory, this is *the* software renderer for every
potato platform: vertex transform, clipping, face building, the ordering table, shadow and
sprite setup, HUD bars, `renderBackground`. It is pure C++ and platform-independent — except
that at the bottom of its declarations it does (`src/platform/gba/render.iwram.cpp:61` on
this branch):

```cpp
#include "rasterizer.h"
```

**Layer 2 — the rasterizer:** each platform provides its own `rasterizer.h` with the span
fillers (`rasterizeF`, `rasterizeGT`, `rasterizeSprite`, ...) matched to its pixel format
and CPU: GBA has ARM assembly for MODE4 (240x160), 32X has SH-2 code, DOS has a pure-C++
MODE13 (8bpp, 320-wide) version — the one we based ours on.

The binding between the layers is the **quoted-include copy trick**. In C++, a quoted
`#include "rasterizer.h"` is searched *relative to the directory of the file containing the
include* before any include path. So the same `render.iwram.cpp`, byte-identical, compiles
against a different rasterizer depending on *which directory you copy it into*. Upstream
platforms do exactly that copy at build time:

```
src/platform/dos/deploy.bat:2:   copy ..\gba\render.iwram.cpp render.cpp /Y
src/platform/tns/Makefile:39:    cp ../gba/render.iwram.cpp render.cpp
```

Our two builds follow the same pattern. The Windows simulator's Makefile
(`src/platform/esp32/win/Makefile:49`):

```make
# the generic renderer MUST be compiled from THIS directory so that its
# quoted #include "rasterizer.h" picks up our local forwarder to the
# shared MODE13 rasterizer (same copy trick as dos/deploy.bat and tns/Makefile)
render.cpp: $(GBA)/render.iwram.cpp
	cp $(GBA)/render.iwram.cpp render.cpp
```

and the firmware does it at CMake configure time
(`src/platform/esp32/idf/main/CMakeLists.txt:18`):

```cmake
# configure-time copy (same pattern as dos/deploy.bat and tns/Makefile)
configure_file(
    ${ENGINE_DIR}/platform/gba/render.iwram.cpp
    ${RENDER_COPY}
    COPYONLY
)
```

In both of our build directories, the local `rasterizer.h` is not the rasterizer itself but
a four-line forwarder (`src/platform/esp32/idf/main/rasterizer.h`, identically
`src/platform/esp32/win/rasterizer.h`):

```cpp
// Forwarder: render.cpp (configure-time copy of gba/render.iwram.cpp) does
// #include "rasterizer.h" which resolves to this directory — forward to the
// shared ESP32 MODE13 rasterizer (same one the Windows sim executes).
#include "../../rasterizer.h"
```

so the simulator and the firmware execute the *same* rasterizer source,
`src/platform/esp32/rasterizer.h` — a single point of truth, which is what makes the
Windows build a byte-exact reference. The important discipline: `render.cpp` in a platform
directory is a **generated file, never edited** (the sim's `.gitignore` excludes its copy;
the firmware's checked-in copy is overwritten by `configure_file` at every configure). Fix
bugs in `gba/render.iwram.cpp` or in the shared rasterizer, never in the copy.

Why does upstream do it this way instead of an include path or an `#ifdef`? Because it lets
one ~1200-line renderer serve N platforms with zero per-platform conditionals in the renderer
itself, while each platform keeps a freely-shaped rasterizer (asm, C, different strides)
under a fixed contract: the function-name macros `#define rasterizeF rasterizeF_c` etc.
that every `rasterizer.h` provides.

## 5. Translation units of a potato build

A striking property of the engine: it is *almost entirely header-only*. `game.h` includes
the whole game — `common.h`, `room.h`, `camera.h`, `item.h`, `draw.h`, `nav.h`, `level.h`,
`inventory.h` (`src/fixed/game.h:4-11`) — as inline definitions. A complete potato build is
only four to five translation units:

| TU | Source | Contents |
|---|---|---|
| platform main | `win/main.cpp` or `idf/main/game_main.cpp` | platform shell + `#include "game.h"` = the entire game logic |
| render | `render.cpp` — generated copy of `gba/render.iwram.cpp` | generic renderer + (via quoted include) the platform rasterizer |
| sound | `src/platform/gba/sound.cpp` | the GBA mixer (AD4 music + PCM SFX), pure-C path when `USE_ASM` is off |
| globals | `src/fixed/common.cpp` | engine global variable definitions (`gSaveGame`, `gLevelInfo`, matrices...) |
| display (ESP32 only) | `idf/main/display_esplcd.c` | ST7789 DMA pipeline behind the 3-function `display.h` interface |

This is exactly the layout the firmware component registers
(`src/platform/esp32/idf/main/CMakeLists.txt:24-38`), and — minus the display TU, plus
Win32 libs — the simulator's `OBJS = main.o sound.o render.o common.o`. Reusing
`gba/sound.cpp` verbatim was free: like the renderer, it is generic C++ with the
platform-specific parts (ASM mixing) behind `USE_ASM`, which we do not define.

## 6. The platform ladder pattern in `src/fixed/common.h`

`common.h` is the engine's single configuration point, and it is written as a series of
`#if defined(PLATFORM_A) ... #elif defined(PLATFORM_B) ...` chains — *ladders* — that every
platform must join. Adding a platform to the potato engine is precisely the exercise of
finding every ladder and adding your `#elif` rung (plus two ladders that live outside
`common.h`). Miss one and you get either a compile error (best case) or another platform's
behavior silently (worst case — see the `-U__WIN32__` story in `03-windows-simulator.md`:
MinGW predefines `__WIN32__`, which would select the desktop GL branch of the *first*
ladder and change every subsequent one).

The full list of rungs the ESP32 port added, in file order (line numbers from branch
`esp32s3`):

| Ladder (what it selects) | Location | ESP32 rung |
|---|---|---|
| Platform block: mode, resolution, level format, system headers | `common.h:34-63` | `MODE13`, 320x240, `VRAM_WIDTH (FRAME_WIDTH/2)`, `USE_FMT (LVL_FMT_PKD)`, `esp_attr.h`; twin `__ESP32_WIN__` block with `windows.h` |
| `LOG` macro | `common.h:196-213` | none needed after the upstream `LOG()` -> `LOG(...)` fix (see `06-engine-port.md`) |
| Optimization profile ("potato profile") | `common.h:229-238` | full GBA profile copied: `HIDE_CORPSES`, `LOD_TRAP_FLOOR`, `NO_STATIC_MESH_PLANTS`, `MAX_ENEMIES 3`, `VIEW_DIST (10<<10)`, `FAST_HITMASK` |
| Placement `operator new` | `common.h:344-353` | `#include <new>` instead of the engine's inline definition — the only Xtensa-forced code change in the port |
| `int2str` | `common.h:367-368` | `itoa(x, str, 10)` (newlib has `itoa`; the `#else` default `_itoa` is MSVC/Watcom-only) |
| `EWRAM_COLD` (new ladder added by this port) | `common.h:388-392` | `EXT_RAM_BSS_ATTR` (PSRAM `.bss`); `EWRAM_DATA` elsewhere |
| `ASSERT` / `STATIC_ASSERT` | `common.h:394-400` | `__ESP32_WIN__` joins the `DebugBreak()` branch; `__ESP32__` takes the empty default |
| `fb` extern declaration | `common.h:402-414` | `extern uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2]` — right-sized for 8bpp (sim keeps the oversized upstream shape for diff-minimalism) |
| Sound parameters | `common.h:456-463` | joins the `__GBA_WIN__` rung: 22050 Hz, 1024 samples, 8-bit signed encode/decode |
| `MAX_VERTICES` | `common.h:490-496` | `#else` default (5*1024) — no rung needed |
| `renderInit`/`renderFree`/`renderSwap`/`renderLevelInit`/`renderLevelFree` | `common.h:2871-2883` | joins the GBA rung where all five are no-op macros (software renderer needs no GAPI lifecycle) |
| `PROFILE_START`/`PROFILE_STOP` | `common.h:3014` | `__ESP32_WIN__` joins the `QueryPerformanceCounter` rung; the `__ESP32__` target has no rung yet (`PROFILING` is off; a rung is needed before Phase 5 profiling — see `08-performance.md`) |
| Input scheme (outside `common.h`) | `src/fixed/lara.h:2748` | joins the GBA chord scheme: A=action, A+L=weapon, B=jump, B+L=roll, R=walk, R+L=look |
| `fb` definition (outside `common.h`) | `src/platform/gba/render.iwram.cpp:28-38` | `uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2]` for `__ESP32__`; full-size for `__ESP32_WIN__` |

Note the deliberate two-rung strategy throughout: `__ESP32_WIN__` (the simulator) joins the
*Windows* rung of host-facing ladders (`ASSERT`, `PROFILE`, `windows.h`) but the *target's*
rung of every engine-behavior ladder (`MODE13`, resolution, PKD, sound rates, input scheme,
render stubs). That is what "byte-exact reference" means in practice: the two builds differ
only where the host OS forces them to.

The `#else` at the bottom of the first ladder is the enforcement mechanism
(`common.h:192-194`):

```cpp
#else
    #error unsupported platform
#endif
```

Every change these rungs required is dissected line by line in `06-engine-port.md`.

## Key takeaways

- OpenLara contains two complete, unrelated engines; only `src/fixed/` — pure fixed-point
  (`FIXED_SHIFT 14`), 8bpp paletted, proven on a 16.78 MHz GBA — is portable to an MCU.
  The port did not shrink the desktop engine; it re-homed the GBA one.
- The potato engine is 32-bit-pointer-only by design (`*ptr += (uint32)data` offset
  patching, pointer color-smuggling). The 32-bit little-endian Xtensa LX7 matches the GBA's
  ARM7TDMI, so even the `.PKD` level files load unmodified.
- 8bpp is not a compromise but the architecture: the palette *is* the lighting model
  (`gLightmap`), the framebuffer fits internal SRAM (76.8 KB), and palette-to-RGB565
  expansion is deferred to the DMA display flush.
- The port is conceptually a memory re-mapping: cart ROM -> mmap'd flash, EWRAM -> octal
  PSRAM via the new `EWRAM_COLD` macro (~130 KB of SRAM freed), IWRAM -> internal SRAM.
  Levels are additionally copied to PSRAM because it serves random misses 3-4x faster
  than flash.
- One generic renderer (`gba/render.iwram.cpp`) serves all potato platforms by being
  *copied* next to each platform's `rasterizer.h`, exploiting quoted-include resolution.
  Both ESP32 builds copy it at build time and forward to a single shared rasterizer,
  which is why the Windows simulator is a faithful reference.
- A potato build is 4-5 TUs: platform main (whole game via header-only `game.h`), the
  renderer copy, the GBA sound mixer, engine globals, and (on ESP32) the display driver.
- Porting the engine = joining every `#elif` ladder in `common.h` plus two outside it
  (`lara.h` input scheme, `render.iwram.cpp` fb definition). The ladders are enumerable,
  finite, and end in `#error unsupported platform` — the honest definition of the porting
  surface.
