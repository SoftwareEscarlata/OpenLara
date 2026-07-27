# 06 — Porting the Engine: Every Change Explained

This document walks through **every modification the port made to engine code**, with
before/after excerpts and the reasoning for each — first the shared engine files
(`src/fixed/`, `src/platform/gba/render.iwram.cpp`), then the ESP32 rasterizer's three
upstream bug fixes, then the firmware platform layer `game_main.cpp` section by section.
The headline is how *little* changed: across roughly 100k lines of engine code, the diff
against upstream `master` touches six engine files, and only **one** change was forced by
the Xtensa CPU itself. Everything else is memory layout and platform ladders (the ladder
concept is introduced in `01-architecture.md`).

The complete engine-side diff (`git diff master...esp32s3`):

| File | Lines changed | Nature |
|---|---|---|
| `src/fixed/common.h` | +68/-7 | platform blocks + ladder rungs + `EWRAM_COLD` + two upstream bug fixes |
| `src/fixed/common.cpp` | 1 | `gSaveData` -> `EWRAM_COLD` |
| `src/fixed/enemy.h` | 1 | `enemiesExtra` -> `EWRAM_COLD` |
| `src/fixed/room.h` | 1 | `dynSectors` -> `EWRAM_COLD` |
| `src/fixed/lara.h` | 1 | join the GBA input-scheme ladder |
| `src/platform/gba/render.iwram.cpp` | +4/-2 | `fb` definition rungs + `gBackgroundCopy` -> `EWRAM_COLD` |

Everything else the port added is *new* files under `src/platform/esp32/`.

## 1. `src/fixed/common.h`

### 1.1 The `__ESP32_WIN__` and `__ESP32__` platform blocks

The first ladder in `common.h` selects the platform. Upstream begins with `__WIN32__`; the
port inserts two rungs *before* it (order matters — see the `-U__WIN32__` note below):

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
    #define FRAME_HEIGHT 240
    #define VRAM_WIDTH   (FRAME_WIDTH / 2)

    #define USE_FMT     (LVL_FMT_PKD)

    #include <stdlib.h>
    #include <stdio.h>
    #include "esp_attr.h"   // EXT_RAM_BSS_ATTR for EWRAM_COLD
#elif defined(__WIN32__)
    ...
```

Decisions encoded here:

- **`MODE13`, 320x240** — the 8bpp paletted profile, same as DOS and TI-Nspire, at the
  panel's native resolution. Every other choice in the port flows from this one.
- **`VRAM_WIDTH (FRAME_WIDTH / 2)`** — the framebuffer row stride in `uint16` units.
  Upstream `MODE13` code *uses* `VRAM_WIDTH` but no header in the whole repository
  *defines* it (see the rasterizer section below); defining it in the platform block fixes
  the DOS-lineage code for both of our builds.
- **`USE_FMT (LVL_FMT_PKD)`** — the GBA's repacked level format. GBA is ARM little-endian
  32-bit; Xtensa LX7 is little-endian 32-bit. The `.PKD` files from
  `src/platform/gba/data/` load unmodified (verified in code and at runtime on both x86
  Win32 and the board).
- **The two blocks are deliberate twins.** The simulator differs only in host plumbing
  (`windows.h`); everything the *engine* sees — mode, sizes, format — is identical, which
  is what makes the sim a byte-exact reference (`03-windows-simulator.md`).
- **What is *not* here:** `ROM_READ` (GBA defines it; we keep the level writable in PSRAM
  and re-copy it pristine per load — section 8.2), and `USE_ASM` (all-C paths everywhere).

One trap discovered the hard way: MinGW g++ *predefines* `__WIN32__`, so the simulator must
compile with `-U__WIN32__` or this ladder silently selects the desktop GL/PHD branch. The
simulator's Makefile does this; the rungs being first also means an explicitly-defined
`__ESP32_WIN__` always wins.

### 1.2 The `LOG(...)` fix (upstream bug #1 in this file)

```diff
 #else
-    #define LOG()
+    #define LOG(...)
 #endif
```

Upstream's release-mode `LOG` is declared with *zero* parameters but called with arguments
throughout the engine (`LOG("time: %d\n", t)`). MSVC's preprocessor tolerates that; GCC
does not — every non-`_DEBUG` g++ build of the potato engine dies at the first `LOG` call
site. The variadic form `LOG(...)` swallows any argument list and expands to nothing, which
is what upstream meant. This surfaced in the first NDEBUG simulator build and would equally
break the Xtensa build (also GCC).

### 1.3 The potato profile block

```cpp
// Optimization flags =========================================================
#ifdef __ESP32__
// start with the proven GBA potato profile; the S3 has CPU headroom to relax
// these later (measured, one at a time)
    #define HIDE_CORPSES (30*10) // 10 sec
    #define LOD_TRAP_FLOOR
    #define NO_STATIC_MESH_PLANTS
    #define MAX_ENEMIES 3
    #define VIEW_DIST (10 << 10)
    #define FAST_HITMASK
#endif
```

This is the GBA's block (`common.h:240-253`) copied verbatim: hide corpses after 10
seconds, LOD the trap-floor geometry, drop transparent plant meshes, cap active enemies at
3, cut view distance, skip per-sphere enemy collision. These are *free, proven* switches —
the GBA shipped with them. The S3 has roughly 14x the single-core throughput, so several
can probably be relaxed; the plan (Phase 5, `08-performance.md`) is to relax them one at a
time against measurements, never on faith.

One divergence to be aware of: the block is guarded by `#ifdef __ESP32__` only, and the
simulator defines `__ESP32_WIN__`, not `__ESP32__` — so the sim runs the engine's *default*
profile (`MAX_ENEMIES 8`, corpses never hidden, full trap-floor and plant geometry, exact
sphere hit tests; `VIEW_DIST` happens to be identical either way, since the engine-wide
default is also `1024 * 10`). It is the one engine-visible difference
between the two builds: the sim exercises the logic at full load while the target trades
load for frame rate. Keep it in mind when comparing behavior between sim and board.

### 1.4 The placement-new fix — the only Xtensa code change

The story deserves telling in full, because it is the entire answer to "what did the CPU
architecture cost you?".

Upstream `common.h` defines placement `operator new` *itself*, with the real `<new>`
commented out:

```cpp
//#include <new>
inline void* operator new(size_t, void *ptr)
{
    return ptr;
}
```

This is a GBA-ism: freestanding console toolchains (devkitARM and friends) may not ship a
usable `<new>`, and the engine needs placement new for constructing items in static
buffers. Defining it by hand is legal — *as long as nothing else declares it too*.

On ESP-IDF's Xtensa toolchain, the C++ standard library is full libstdc++, and the standard
headers that IDF drags in declare placement `operator new` themselves. The engine's own
definition then collides with the toolchain's declaration, and the build fails inside
`common.h` — in ~100k lines of engine code, the only place the Xtensa toolchain said "no".

The fix inverts upstream's choice, only for this platform:

```cpp
#if defined(__ESP32__)
// the xtensa toolchain's headers already declare placement new — use theirs
#include <new>
#else
//#include <new>
inline void* operator new(size_t, void *ptr)
{
    return ptr;
}
#endif
```

Every other platform (including the simulator, whose MinGW build tolerates the inline
definition) keeps upstream behavior. That's it. That is the complete list of code changes
the instruction set, the compiler, and the RTOS forced on the engine: **one `#include`**.
Everything else in this document is memory placement and configuration.

### 1.5 `int2str`

```cpp
#elif defined(__ESP32__)
    #define int2str(x,str) itoa(x, str, 10)
```

The ladder's `#else` default is `_itoa` — an MSVC/Watcom name that newlib (ESP-IDF's libc)
does not have. Newlib has plain `itoa`, the same rung GBA/NDS/32X use. The simulator falls
through to `_itoa`, which MinGW provides. Used by the FPS counter and inventory text.

### 1.6 `EWRAM_COLD` — a new ladder for the memory story

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

Motivation: the first firmware link failed — `.dram0.bss` overflowed `dram0_0_seg` by
**2528 bytes**. The engine's static state plus the display pipeline's DMA buffers simply do
not fit the S3's internal SRAM at 320x240. Rather than shrink the game, the port reuses the
GBA's own hot/cold annotation: anything the GBA already tolerated in slow 16-bit EWRAM is,
by construction, not latency-critical, so the *coldest* of those arrays can live in
cache-backed octal PSRAM. `EXT_RAM_BSS_ATTR` places a zero-initialized array in external
RAM (requires `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` in sdkconfig, see
`07-build-system.md`). On every other platform the macro degrades to `EWRAM_DATA`, making
the annotation invisible upstream.

It was applied to exactly four arrays — chosen by size and coldness, not wholesale:

| Array | Declaration | Size | Why it is cold |
|---|---|---|---|
| `gBackgroundCopy` | `src/platform/gba/render.iwram.cpp:67` | 75 KB | written/read only when the inventory opens (blurred background) |
| `gSaveData` | `src/fixed/common.cpp:18` | 8 KB | touched at save/load only |
| `dynSectors` | `src/fixed/room.h:10` | 24 KB (`MAX_DYN_SECTORS = 3*1024`, the `// EWRAM 8k` comment is stale upstream) | floor-data overrides, sparse access |
| `enemiesExtra` | `src/fixed/enemy.h:12` | ~25 KB at `MAX_ENEMIES 3` | per-enemy pathfinding scratch, touched a few times per tick |

Combined effect: ~130 KB of internal SRAM freed; the link succeeds with 33 KB of internal
heap to spare after full game init. The hot set — `fb`, `gVertices` (40 KB), `gFaces`
(30 KB), `divTable` (2 KB, hit per span), `items` (19.5 KB), `gLightmap` (8 KB), rooms —
deliberately stays internal. The complete placement map and the "framebuffer must never be
in PSRAM" analysis are in `05-memory-map.md`.

The three one-line engine diffs are all of this shape:

```diff
-EWRAM_DATA uint8 gSaveData[SAVEGAME_SIZE - sizeof(SaveGame)];
+EWRAM_COLD uint8 gSaveData[SAVEGAME_SIZE - sizeof(SaveGame)];
```

### 1.7 The remaining ladder rungs

**`ASSERT` (`common.h:394`)** — the simulator joins the Windows rung so a failed assert
breaks into the debugger; the target keeps the empty default (no debugger attached, and
`DebugBreak` does not exist):

```diff
-#if defined(__WIN32__) || defined(__GBA_WIN__)
+#if defined(__WIN32__) || defined(__GBA_WIN__) || defined(__ESP32_WIN__)
     #define ASSERT(x) { if (!(x)) { DebugBreak(); } }
```

**`fb` extern declaration (`common.h:402-414`)** — the target gets a right-sized
framebuffer; the sim keeps upstream's shape:

```cpp
#if defined(__GBA_WIN__) || defined(__ESP32_WIN__)
    extern uint16 fb[FRAME_WIDTH * FRAME_HEIGHT];
#elif defined(__ESP32__)
    // right-sized for 8bpp: W*H bytes = W*H/2 uint16 elements (saves 75KB of SRAM
    // vs the upstream 2x-oversized declaration kept on the sim for diff-minimalism)
    extern uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2];
```

Why upstream is 2x oversized: `uint16 fb[W * H]` is GBA-VRAM legacy (MODE4 addresses VRAM
in 16-bit units and cannot do byte writes; the desktop sims just mirrored the shape). In
8bpp MODE13 the frame is `W*H` *bytes*, i.e. `W*H/2` `uint16` elements. On a desktop the
wasted 75 KB is irrelevant and keeping the shape minimizes the upstream diff; on the S3
those 75 KB are ~17% of usable SRAM. Every access goes through byte or `uint16` pointers
into the array, so only the declared size changes — no code change.

**Sound parameters (`common.h:456`)** — both new platforms join the `__GBA_WIN__` rung:
22050 Hz output/sample rate, 1024-sample buffers, 8-bit with +128 encode bias. Chosen so
the GBA mixer (`gba/sound.cpp`) runs unmodified and the sim's waveOut path is
byte-identical to the future I2S path (Phase 4b, `10-roadmap.md`).

**Render lifecycle stubs (`common.h:2871`)** — software-renderer platforms define the five
GAPI lifecycle functions away as no-op macros; we join them:

```diff
-#if defined(__GBA__) || defined(__GBA_WIN__)
+#if defined(__GBA__) || defined(__GBA_WIN__) || defined(__ESP32_WIN__) || defined(__ESP32__)
 #define renderInit()
 #define renderFree()
 #define renderSwap()
 #define renderLevelInit()
 #define renderLevelFree()
```

There is no GL context to create or swap; the "swap" is `displayFlush(fb)` in the platform
loop.

**`PROFILE_START/STOP` (`common.h:3014`)** — the simulator joins the
`QueryPerformanceCounter` rung:

```diff
-    #elif defined(__WIN32__) || defined(__GBA_WIN__)
+    #elif defined(__WIN32__) || defined(__GBA_WIN__) || defined(__ESP32_WIN__)
```

The `__ESP32__` target deliberately has no rung yet: `PROFILING` is off by default, so the
poison `#else` (`#define PROFILE_START() aaa`) never compiles. Before Phase 5 profiling on
hardware, the target needs an `esp_timer`/cycle-counter rung — noted as pending in
`08-performance.md`.

## 2. `src/fixed/lara.h` — joining the input scheme ladder

One line (`lara.h:2748`):

```diff
-    #elif defined(__GBA__) || defined(__GBA_WIN__)
+    #elif defined(__GBA__) || defined(__GBA_WIN__) || defined(__ESP32__) || defined(__ESP32_WIN__)
```

`Lara::updateInput` maps platform `keys` (`IK_*` bits) to game actions (`IN_*`) with a
per-platform scheme, because platforms have different button counts. Our pad is GBA-shaped
(D-pad + A/B/L/R/START/SELECT), so we take the GBA *chord* scheme, where `L` is a modifier:

```cpp
if (keys & ikA)
{
    if (keys & IK_L) {
        if (extraL->weaponState != WEAPON_STATE_BUSY) {
            input |= IN_WEAPON;      // A+L = draw/holster weapon
        } else {
            input |= IN_ACTION;
        }
    } else {
        input |= IN_ACTION;          // A = action
    }
}

if (keys & ikB)
{
    if (keys & IK_L) {
        input |= IN_UP | IN_DOWN;    // B+L = roll
    } else {
        input |= IN_JUMP;            // B = jump
    }
}

if (keys & IK_R)
{
    if (keys & IK_L) {
        input |= IN_LOOK;            // R+L = look
    } else {
        input |= IN_WALK;            // R = walk
    }
}
```

So: A=action, A+L=weapon, B=jump, B+L=roll, R=walk, R+L=look — the exact GBA port
controls. (`ikA`/`ikB` respect the `controls_swap` setting.) TR1 on PC used ~12 distinct
keys; chords are how the GBA port compressed that onto 6 buttons, and we inherit the
compression for free.

## 3. `src/platform/gba/render.iwram.cpp` — two changes to the generic renderer

The generic renderer (see `01-architecture.md` for its role and the copy trick) needed a
`fb` definition rung and one `EWRAM_COLD` annotation:

```diff
-#if defined(__GBA_WIN__)
+#if defined(__GBA_WIN__) || defined(__ESP32_WIN__)
     uint16 fb[FRAME_WIDTH * FRAME_HEIGHT];
+#elif defined(__ESP32__)
+    uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2]; // 8bpp: 2 pixels per element
 #elif defined(__GBA__)
     uint32 fb = MEM_VRAM;
```

This is the *definition* matching the `common.h` extern (section 1.7): full-size on the sim
(upstream shape), right-sized on the target, and note the GBA's own rung — there `fb` is
not an array at all but the literal VRAM address. The renderer writes pixels through
pointers derived from `fb` in all cases, which is why the shape can vary per platform
without touching any drawing code.

```diff
-EWRAM_DATA uint8 gBackgroundCopy[FRAME_WIDTH * FRAME_HEIGHT];   // EWRAM 37.5k
+EWRAM_COLD uint8 gBackgroundCopy[FRAME_WIDTH * FRAME_HEIGHT];   // EWRAM 37.5k
```

`gBackgroundCopy` is the single biggest cold array (75 KB at 320x240 — the `37.5k` comment
is the GBA's 240x160). It exists only for the inventory: `copyBackground()`
(`render.iwram.cpp:1194`) snapshots the framebuffer into it and gray-remaps it for the
inventory backdrop. Two touches per inventory open — the definition of cold. PSRAM's
sequential copy speed (~84 MB/s) makes the snapshot invisible.

## 4. `src/platform/esp32/rasterizer.h` — three upstream bugs fixed

The port needed a pure-C++ MODE13 (8bpp) rasterizer. Upstream has exactly one:
`src/platform/dos/rasterizer.h`. It is also visibly bitrotted — the DOS build has not
compiled for some time. `src/platform/esp32/rasterizer.h` is that file plus three fixes,
declared at the top of the file:

```cpp
// MODE13 (8bpp paletted) pure-C++ rasterizer for the ESP32-S3 port.
// Based on platform/dos/rasterizer.h with fixes:
//  - VRAM_WIDTH defined (row stride in uint16 units; undefined in the DOS tree)
//  - rasterizeF_c: missing `R = L;` reset after color extraction (crash on flat faces)
//  - rasterizeSprite/LineH/LineV/FillS ported from platform/gba/rasterizer.h (were TODO stubs)
```

### 4.1 `VRAM_WIDTH` is used but defined nowhere

Every span function in the DOS rasterizer steps to the next scanline with:

```cpp
pixel += VRAM_WIDTH;
```

(`dos/rasterizer.h:121, 227, 352, 511, 659`). Search the entire repository: **no header
defines `VRAM_WIDTH`**. The DOS build simply does not compile upstream — proof of bitrot,
and a warning that the DOS tree could not be trusted blindly. Since `pixel` is a `uint16*`
into an 8bpp frame, the stride is `FRAME_WIDTH` *bytes* = `FRAME_WIDTH/2` `uint16`
elements:

```cpp
#ifndef VRAM_WIDTH
    #define VRAM_WIDTH (FRAME_WIDTH / 2)
#endif
```

Defined both here (self-containment) and in the `common.h` platform blocks.
Failure mode if wrong: compile error if undefined; skewed/sheared geometry if mis-defined.

### 4.2 `rasterizeF_c`: the missing `R = L;` (crash on flat-shaded faces)

The renderer's flat-color path smuggles the face color *through the `R` pointer
parameter*: instead of a right-edge vertex pointer, `R` carries a color index 0..255 cast
to `VertexLink*`. The rasterizer extracts it first thing:

```cpp
void rasterizeF_c(uint16* pixel, const VertexLink* L, const VertexLink* R)
{
    uint32 color = (uint32)R;
    color = gLightmap[(L->v.g << 8) | color];
    color |= (color << 8);
    ...
```

After extraction, `R` must be re-pointed at the real polygon before the edge walkers run.
The GBA C rasterizer does this (`gba/rasterizer.h:167`):

```cpp
    R = L;
```

The DOS version *lost that line*. Its edge loop then walks `R + R->next` — where `R` is the
integer 0..255 reinterpreted as an address. Reading edge links from near-NULL memory
produces garbage heights and coordinates; on a machine with memory protection it crashes
outright. The failure mode is vicious in practice because flat-shaded triangles are
comparatively rare in TR1 (most surfaces are textured), so the bug fires seemingly at
random. The port restores the line with a comment
(`src/platform/esp32/rasterizer.h:152`):

```cpp
    R = L; // R carried the color index, not a vertex — restart both edge walkers at L
```

Both edge walkers starting from `L` is correct: `prev`/`next` links traverse the same
polygon in opposite directions.

### 4.3 Sprites, lines and fills were empty `TODO` stubs

The DOS file ends with:

```cpp
void rasterizeSprite_c(uint16* pixel, const VertexLink* L, const VertexLink* R)
{
    // TODO
}
```

— and identical stubs for `rasterizeLineH_c`, `rasterizeLineV_c`, `rasterizeFillS_c`
(`dos/rasterizer.h:674-692`). Failure mode: no crash, just silent absence of everything
those four draw — **no pickups, no muzzle flashes, no menu text glyphs (text is drawn as
sprites), no health/air bars (LineH/LineV borders + FillS fills), no inventory background
dimming**. The game runs but is unplayable-blind.

The port implements all four (`src/platform/esp32/rasterizer.h:688-887`) by porting the
**C versions from `gba/rasterizer.h`** (the MODE4 build's fallback path when `USE_ASM` is
off). These are stride-correct for any `FRAME_WIDTH` because they step scanlines in bytes:

```cpp
        ptr += FRAME_WIDTH;      // sprite/lineV: byte pointer, works at 240 or 320
...
        pixel += FRAME_WIDTH / 2; // fillS: uint16 pointer, same stride in elements
```

The only subtlety in the port is the even/odd-address handling (`alignL`/`alignR` in
`rasterizeSprite_c`): the framebuffer is written through 16-bit stores (GBA VRAM cannot do
byte writes, and the convention is kept), so a sprite starting or ending on an odd byte
must read-modify-write the neighboring pixel. The GBA logic carries over unchanged.

## 5. `src/platform/esp32/idf/main/game_main.cpp` — the firmware platform layer

`game_main.cpp` (290 lines) is the entire firmware-side platform layer: it implements the
`os*` contract that `game.h` expects, mounts level data, reads input, and runs the game
loop. It deliberately mirrors the Windows harness (`win/main.cpp`) — same globals, same
tick math — replacing Win32 calls with ESP-IDF equivalents. Its own header comment is the
map:

```cpp
//   file loads  -> OLVL container in the mmap'd "levels" flash partition,
//                  level blob memcpy'd to PSRAM (writable; engine mutates it)
//   blit        -> displayFlush(fb) through the esp_lcd DMA pipeline
//   settings    -> NVS (stubbed to defaults for now)
```

### 5.1 Mounting the OLVL container

There is no filesystem. Levels live in a raw flash partition (`levels`, data subtype
`0x40`, offset `0x310000`, 12.9 MB — see `partitions.csv`), formatted as an **OLVL
container** built by `make_levels.py`: magic `'OLVL'` (`0x4C564C4F`), a file count, then
`{char name[16]; uint32 offset; uint32 size}` entries, with every blob 4-byte aligned so
mmap'd pointers are directly usable.

```cpp
static bool levelsMount()
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "levels");
    if (!part) return false;

    const void* map;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h) != ESP_OK)
        return false;

    const uint32* hdr = (const uint32*)map;
    if (hdr[0] != OLVL_MAGIC) { ... return false; }

    sLevelsBase = (const uint8*)map;
    sFileCount  = hdr[1];
    sFiles      = (const OlvlEntry*)(sLevelsBase + 8);
    return true;
}
```

`esp_partition_mmap` maps the whole partition into the data address space through the
cache — this is the "cart ROM" of the GBA mapping (`01-architecture.md`). `levelsFind()`
is a linear `strcmp` over the entries; with five files, a directory structure would be
ceremony.

### 5.2 `osLoadLevel` — why memcpy to PSRAM, and why a pristine copy every time

```cpp
const void* osLoadLevel(LevelID id)
{
    char buf[32];
    sprintf(buf, "%s.PKD", (const char*)gLevelInfo[id].data);

    const OlvlEntry* e = levelsFind(buf);
    if (!e) { ... return NULL; }

    // the engine mutates the level image (tile fixups, animated textures) and
    // needs a pristine copy per (re)load — free the old one, copy from flash.
    // PSRAM serves random texture reads 3-4x faster than flash and keeps the
    // flash side of the shared SPI0 bus free for code fetch.
    free(levelData);
    levelData = (uint8*)heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM);
    ...
    memcpy(levelData, sLevelsBase + e->offset, e->size);
    return levelData;
}
```

Two independent reasons force this design, and both are worth understanding:

**Why copy to PSRAM at all (performance)?** The engine could read the level straight from
the mmap'd flash pointer — that is literally what the GBA does with the cart. But on the
S3, flash and PSRAM share the SPI0 bus and the same D-cache with an arbiter between them,
and a random cache-line miss is served in ~0.5 us from octal PSRAM versus ~1.8-2 us from
quad flash — 3-4x faster. Texture sampling during rasterization is exactly that random
access pattern. Worse, code also executes from flash through the same cache, so texture
misses from flash would contend with instruction fetch. Copying once at load (measured:
TITLE.PKD, 306 KB, **16 ms**) buys back that margin every frame thereafter. Sources for
all numbers: `09-research-notes.md`.

**Why a *pristine* copy per level start (correctness)?** The PKD format is loaded by
casting structures in place — and then **mutated**, twice over:

- At load: `read_PKD` patches tile *pointers* into the blob's texture records
  (`src/fixed/fmt/pkd.h:74`): `level.textures[i].tile += (uint32)level.tiles;` — after
  which those fields hold absolute addresses valid only for this allocation.
- At runtime: `animTexturesShift()` (`src/fixed/level.h:211`, called every 5 game frames
  from `updateLevel`) *rotates texture records inside the blob* to animate lava, water,
  etc. Trigger handling also flips camera flags in floor data.

Reload the same blob without restoring it and the second `read_PKD` adds base addresses to
already-patched pointers; animated textures resume mid-rotation. The GBA never sees this
because its "blob" is immutable cart ROM and `ROM_READ` copies the mutable tables aside.
Our solution is simpler than `ROM_READ`: keep the flash copy immutable (it is), and
`free` + `memcpy` a fresh writable copy per `osLoadLevel`. At 16 ms per load, pristine
correctness is essentially free, and `ROM_READ`'s table-copy machinery stays off.

`gLevelInfo[id].data` holds the level's base name (`"TITLE"`, `"GYM"`, ...) — the same
table the simulator uses to build `data/%s.PKD` paths.

### 5.3 The `os*` stubs and the title screen

```cpp
bool osSaveSettings() { return false; }
bool osLoadSettings() { return false; }
bool osCheckSave()    { return false; }
bool osSaveGame()     { return false; }
bool osLoadGame()     { return false; }
void osJoyVibrate(int32 index, int32 L, int32 R) {}
```

Settings and saves return `false` — the engine treats that as "no save present / use
defaults" and proceeds. The honest implementations (NVS-backed, mirroring the simulator's
`settings.dat`/`savegame.dat` file versions in `win/main.cpp:50-110`) are Phase 6 work
(`10-roadmap.md`). `osGetSystemTimeMS` is `esp_timer_get_time()/1000`; `osSetPalette`
forwards to `displaySetPalette` — note the engine calls it every fade tick (`palSet` runs
per `updateFading` frame), so the display layer treats palette upload as a hot path.

The title screen background is a placeholder with a precise reason:

```cpp
// no 320x240 TITLE.SCR asset yet -> black background (PSRAM, zeroed)
TITLE_SCR = heap_caps_calloc(1, FRAME_WIDTH * FRAME_HEIGHT, MALLOC_CAP_SPIRAM);
```

`renderBackground` (`render.iwram.cpp:1189`) does `dmaCopy(background, (void*)fb,
FRAME_WIDTH * FRAME_HEIGHT)` — exactly W*H bytes, no scaling. The GBA's `TITLE.SCR` asset
is 240x160 = 38,400 bytes; feeding it to a 76,800-byte copy would read past the asset.
Until a 320x240 asset is regenerated with the GBA packer (Phase 6), a zeroed PSRAM buffer
gives a black backdrop, and the title *menu* renders over it normally.

### 5.4 Input: three concurrent sources OR'd into `keys`

The engine consumes one `uint32 keys` bitfield of `IK_*` flags (`common.h:588-606`).
`inputUpdate()` merges three sources every frame — any of them can drive the game,
simultaneously:

**Source 1 — serial over USB-Serial-JTAG** (play from the PC with no wiring):
characters map to key bits —

| Char | Bit | Key | Char | Bit | Key |
|---|---|---|---|---|---|
| `w` | 0 | `IK_UP` | `x` | 4 | `IK_A` (action) |
| `d` | 1 | `IK_RIGHT` | `z` | 5 | `IK_B` (jump) |
| `s` | 2 | `IK_DOWN` | `q` | 10 | `IK_L` (chord) |
| `a` | 3 | `IK_LEFT` | `e` | 11 | `IK_R` (walk) |
| Enter | 14 | `IK_START` | space | 15 | `IK_SELECT` |

A serial stream has key-*down* events only — there is no key-up. The **hold mechanism**
synthesizes release: each received char reloads a per-bit countdown, and a bit stays
pressed while its counter is nonzero:

```cpp
#define SERIAL_HOLD_FRAMES 8

static uint8 sHold[16]; // per-IK_* bit countdown

    // on receive:
    if (bit >= 0) sHold[bit] = SERIAL_HOLD_FRAMES;

    // per frame:
    for (int i = 0; i < 16; i++) {
        if (sHold[i]) { sHold[i]--; k |= 1u << i; }
    }
```

Eight frames (~130-160 ms at 50-60 fps) matches typical terminal auto-repeat, so holding
`w` in miniterm produces a continuous run instead of a stutter. Chords work because two
counters can be live at once (`x` then `q` within 8 frames = A+L = draw weapon).

**Source 2 — the physical pad** on header P1 (see `02-hardware.md` for wiring): a
gpio-to-bit table, active low with internal pull-ups (`IO16`/`IO21` also have 4.7K
hardware pull-ups on the board, borrowed from the camera's SCCB):

```cpp
static const struct { uint8 gpio; uint32 mask; } sButtons[] = {
    { PIN_BTN_UP,     IK_UP     },
    ...
    { PIN_BTN_SELECT, IK_SELECT },
};
    // per frame:
    if (!gpio_get_level((gpio_num_t)sButtons[i].gpio)) k |= sButtons[i].mask;
```

**Source 3 — the BOOT button** (GPIO0, always present on the board):

```cpp
    // BOOT button (active low) = START+A, still works standalone
    if (!gpio_get_level(GPIO_NUM_0)) k |= IK_START | IK_A;
```

START+A is the minimal chord that gets from title screen into gameplay — an emergency
"demo mode" requiring no PC and no soldering.

No debouncing is implemented: the engine samples at frame rate and TR1's input logic is
edge-tolerant; none has proven necessary in play so far.

### 5.5 The game task and loop timing

```cpp
extern "C" void app_main(void)
{
    // 32KB stack, internal RAM, pinned to core 0 (core 1 reserved for
    // audio mixing + future display offload)
    xTaskCreatePinnedToCore(gameTask, "game", 32 * 1024, NULL, 5, NULL, 0);
}
```

The game runs in one FreeRTOS task pinned to **core 0**, priority 5, with a 32 KB stack
(the engine's recursion — room traversal, nested matrix stacks — overflows the default
sizes). Core 1 is intentionally left idle for Phase 4b/5 (audio mixing, display offload).

Boot order inside `gameTask`: `levelsMount()` -> resolve `TRACKS_AD4` (kept as a flash
mmap pointer — sequential access) -> allocate the zeroed `TITLE_SCR` -> `displayInit()` ->
`inputInit()` -> log free heap -> `gameInit()` -> loop.

The loop is the simulator's loop with IDF primitives:

```cpp
    int64_t startTime = esp_timer_get_time() / 1000 - 33;
    int32 lastFrame = 0;

    while (1) {
        inputUpdate();

        int32 frame = (int32)((esp_timer_get_time() / 1000 - startTime) / 33);
        gameUpdate(frame - lastFrame);
        lastFrame = frame;

        gameRender();

        displayFlush((const uint8_t*)fb);
        ...
        vTaskDelay(1);
    }
```

The timing model matters: `gameUpdate(n)` advances game logic by `n` **33 ms ticks** —
the engine's fixed 30 Hz logic rate, unchanged since the GBA. The platform loop free-runs
as fast as render+flush allow (48-63 fps) and hands the engine however many whole 33 ms
ticks have elapsed (usually 0 or 1). Rendering thus runs *faster* than logic, and the
`- 33` in `startTime` pre-loads exactly one tick so the very first `gameUpdate` call gets
1, not 0. This is identical to `win/main.cpp:316-336` minus the sim's debug time-warp
keys.

`vTaskDelay(1)` yields one tick — 1 ms, because `CONFIG_FREERTOS_HZ=1000` — so the IDLE
task can run and feed the watchdog; without it a priority-5 busy loop would starve core 0.
The FPS counter accumulates per wall-clock second into the engine-visible `fps` global
(the in-game FPS display reads it). Measured: 62-63 fps on the title screen (display
ceiling — flush is 16.37 ms), 48-50 fps in GYM.

## Key takeaways

- The entire engine-side diff is six files; four of the changes are one-liners. The only
  modification forced by the Xtensa architecture in ~100k lines was replacing the engine's
  hand-rolled placement `operator new` with `#include <new>`.
- Two of the changes fix genuine upstream bugs that predate the port: the zero-arg `LOG()`
  macro (breaks every GCC release build) and the DOS rasterizer's missing `R = L;`
  (crash on flat-shaded faces, caused by the pointer-smuggled color parameter).
- The DOS tree is bitrotted — `VRAM_WIDTH` is used but defined nowhere in the repository —
  so its rasterizer had to be adopted critically, not copied blindly. The sprite, line and
  fill rasterizers were empty `TODO` stubs and were ported from the GBA C versions; without
  them there are no pickups, no text, and no HUD.
- `EWRAM_COLD` is the port's one new engine concept: reuse the GBA's own hot/cold
  annotations to park four provably-cold arrays (~130 KB) in PSRAM, turning a 2528-byte
  link overflow into 33 KB of headroom, invisible to other platforms.
- The level blob must be re-copied pristine from flash on every level start because the
  engine patches pointers into it at load (`pkd.h`) and rotates texture records inside it
  at runtime (`animTexturesShift`). PSRAM makes the copy 16 ms — cheaper than `ROM_READ`'s
  machinery and immune to reload corruption.
- `game_main.cpp` mirrors the Windows harness deliberately: same globals, same 33 ms tick
  arithmetic, same `os*` contract — so a behavior difference between sim and board points
  at the platform layer, never at the game logic.
- Input is three OR'd sources (serial with an 8-frame hold to synthesize key-up, the P1
  GPIO pad, BOOT as START+A), all feeding one `keys` bitfield that the GBA chord scheme
  in `lara.h` turns into game actions.
- Logic stays at the engine's fixed 30 Hz (33 ms ticks) while rendering free-runs at
  48-63 fps; `vTaskDelay(1)` per frame keeps the watchdog fed with 1 ms granularity.
