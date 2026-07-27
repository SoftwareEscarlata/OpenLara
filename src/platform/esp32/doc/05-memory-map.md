# Making 380KB Fit: the Memory Architecture

This document explains how the OpenLara "potato" engine — whose static state adds up to
roughly 380 KB — fits into an ESP32-S3 whose internal SRAM must also hold the framebuffer,
the display DMA buffers, FreeRTOS, and the ESP-IDF runtime. You will learn what the S3's
memory landscape actually looks like (it is not "512 KB of RAM plus 8 MB of RAM"), how each
engine array was classified as *hot* or *cold*, why the placement macro is selective rather
than wholesale, and how the level data flows from flash to PSRAM and back on every reload.
Every number here was either measured on the physical board or computed from the structs in
the source; file references are relative to the repo root.

The companion status document is [10-roadmap.md](10-roadmap.md).

## 1. The ESP32-S3 memory landscape

The Waveshare ESP32-S3-Touch-LCD-2 carries an ESP32-S3R8: dual Xtensa LX7 at 240 MHz,
512 KB of internal SRAM, 8 MB of octal DDR PSRAM in-package, and 16 MB of quad SPI flash
(W25Q128, QIO at 80 MHz).

| Memory | Size | Latency character | What we put there |
|---|---|---|---|
| Internal SRAM | 512 KB total; our config gives 32 KB to I-cache and 64 KB to D-cache, leaving ~416 KB for code-in-IRAM, `.data`/`.bss`, stacks and heap (the "~440 KB usable" figure quoted in 01/03 is before this maxed-out cache carve-out) | Single-cycle | Framebuffer, hot engine state, DMA buffers, stacks |
| Octal PSRAM | 8 MB @ 80 MHz DDR | ~84 MB/s sequential measured; ~0.5 µs per random cache-line miss | Level blob (up to 2.62 MB), cold engine arrays |
| Flash (mmap) | 16 MB, 12.9 MB `levels` partition | ~1.8–2 µs per random cache-line miss | Code, `.rodata`, OLVL container, TRACKS.AD4 |

Three facts about this landscape drive everything below:

1. **Flash and PSRAM share one bus (SPI0) and one 64 KB data cache, with an arbiter
   between them.** This is confirmed by an Espressif engineer in esp-idf issue #14612 —
   in that issue, moving assets from flash to PSRAM recovered a display's effective pixel
   clock from 4 to 16 MHz, purely by relieving the flash side of the bus. Every random
   texture read from flash also competes with instruction fetch, because code executes
   from flash through the same cache.
2. **Octal PSRAM serves a cache miss 3–4x faster than quad flash** (~0.5 µs vs ~1.8–2 µs
   per line). Sequential PSRAM reads measure ~84 MB/s at 80 MHz DDR (esp32.com t=29970);
   random access over a 4 MB span collapses to ~8.2 MB/s (elect-gombe's Qiita benchmark,
   measured at 120 MHz). PSRAM is "slow RAM", not "fast disk" — and definitely not SRAM:
   memcpy DRAM-to-DRAM measures 358.7 MB/s vs 34.8 MB/s PSRAM-to-PSRAM (esp32.com t=25215).
3. **The cache line must be 64 bytes.** Octal DDR PSRAM uses 64-byte wrap bursts; running
   it with a 32-byte cache line is documented to corrupt the cache (arduino-esp32 #12480).
   So `CONFIG_ESP32S3_DATA_CACHE_LINE_64B` in `src/platform/esp32/idf/sdkconfig.defaults`
   is a correctness requirement, not a tuning knob (it also measures +49% on sequential
   throughput). The same file sets `DATA_CACHE_64KB` + `8WAYS`, which are *not* IDF
   defaults.

## 2. The GBA gives us the vocabulary

The potato engine (`src/fixed/`) was written for the Game Boy Advance, which has a
three-tier memory system, and the code is already annotated for it:

| GBA tier | Size | Character | ESP32-S3 equivalent |
|---|---|---|---|
| IWRAM | 32 KB | Fast, on-die | Internal SRAM |
| EWRAM | 256 KB | Slow (2-cycle, 16-bit bus) | Octal PSRAM — via the new `EWRAM_COLD` macro |
| Cart ROM | up to 32 MB | Read-only, mapped | Flash `esp_partition_mmap` |

The engine tags its arrays with `IWRAM_DATA` / `EWRAM_DATA` macros. On every non-GBA
platform these historically expanded to nothing (`src/fixed/common.h:373-383`), so the
annotations are documentation — but they are *load-bearing documentation*: XProger already
did the hot/cold analysis for a machine far weaker than ours. The port's job was to re-use
that analysis without repeating its one mismatch (see section 5).

## 3. The `.bss` inventory

What actually has to fit. Sizes computed from the structs at our 320x240 / `__ESP32__`
profile (`MAX_ENEMIES 3`, `VIEW_DIST (10 << 10)` — `src/fixed/common.h:229-238`). Beware
that the inline `// EWRAM 37.5k`-style comments upstream are GBA-era (240x160) and
undercount at our resolution.

| Symbol | Defined at | Size (bytes) | Touched | Hot/Cold | Lives in |
|---|---|---|---|---|---|
| `fb` | `src/platform/esp32/idf/main/render.cpp:31` | 76,800 | Every rasterized span | Hot | Internal SRAM |
| `gVertices` | `render.cpp:68` (`Vertex[5120]`, 8 B each) | 40,960 | Every transformed vertex, every frame | Hot | Internal SRAM |
| `gFaces` | `render.cpp:69` (`Face[1920]`, 16 B each) | 30,720 | Every face queued/sorted, every frame | Hot | Internal SRAM |
| `items` | `src/fixed/level.h:17` (`ItemObj[256]`, ~76 B each) | ~19,500 | Game logic, every 33 ms tick | Hot | Internal SRAM |
| `rooms` | `src/fixed/level.h:26` (`Room[139]`, ~64 B each) | ~8,900 | Visibility walk, every frame | Hot | Internal SRAM |
| `gLightmap` | `src/fixed/level.h:13` | 8,192 | Every shaded pixel | Hot | Internal SRAM |
| `gOT` | `render.cpp:70` (`Face*[641]` at `VIEW_DIST 10<<10`) | ~2,600 | Depth-sort buckets, every frame | Hot | Internal SRAM |
| `divTable` | `src/fixed/common.cpp:95` (`uint16[1025]`) | 2,050 | Every span setup (`FixedInvU`) | Hot | Internal SRAM (`.data`, has initializer) |
| `soundBuffer` | `src/platform/gba/sound.cpp:14` | 2,080 | Mixer output | Hot | Internal SRAM |
| `gBackgroundCopy` | `render.cpp:67` | 76,800 | **Only when the inventory opens** (`copyBackground`) | Cold | **PSRAM** (`EWRAM_COLD`) |
| `enemiesExtra` | `src/fixed/enemy.h:12` (3 x ~8.3 KB; each is dominated by `Nav::cells[1024]` = 8 KB) | ~25,000 | AI pathing of up to 3 active enemies | Cold | **PSRAM** (`EWRAM_COLD`) |
| `dynSectors` | `src/fixed/room.h:10` (`Sector[3072]`, 8 B each) | 24,576 | Rooms modified by bridges/trapdoors | Cold | **PSRAM** (`EWRAM_COLD`) |
| `gSaveData` | `src/fixed/common.cpp:18` | ~8,100 | Save/load moments only | Cold | **PSRAM** (`EWRAM_COLD`) |
| assorted small state (`gSpheres`, `playersExtra`, matrices, settings...) | `common.cpp`, `level.h` | ~10–15 K | mixed | Hot-ish | Internal SRAM |

Not in `.bss` but part of the same budget: `gSinCosTable` (16 KB, `const` — lives in flash
`.rodata`), the two 30,720 B display DMA buffers (heap, `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`,
`src/platform/esp32/idf/main/display_esplcd.c:106`), and the 32 KB game task stack
(`game_main.cpp:289`).

The classification rule was frequency, not size: *does the rasterizer or the per-tick logic
touch this array?* If yes, it must sit in single-cycle SRAM, because a PSRAM-resident array
is only fast while it happens to be in the 64 KB D-cache — a cache the textures are already
fighting over.

## 4. The forcing event: a 2528-byte linker error

The first firmware build with the full engine did not fail subtly. It failed like this:

```
.dram0.bss will not fit in region dram0_0_seg
region `dram0_0_seg' overflowed by 2528 bytes
```

That number is deceptively small. It is 2528 bytes over *after* the linker packed
everything perfectly — but a firmware that links with 0 bytes of headroom still cannot
run, because the heap (display buffers, task stacks, esp_lcd internals, the WiFi-free IDF
baseline) comes out of the same region at runtime. The real deficit was on the order of
100+ KB of breathing room. Two changes closed it: halving `fb` (section 6) and `EWRAM_COLD`
(section 5).

## 5. `EWRAM_COLD`: selective, not wholesale

The fix is a one-concept macro in `src/fixed/common.h:385-392`:

```c
// "cold EWRAM": large, rarely-touched arrays that the GBA already parks in its
// slow EWRAM. On ESP32-S3 they live in octal PSRAM (cache-backed) so internal
// SRAM stays free for the framebuffer and hot engine state.
#if defined(__ESP32__)
    #define EWRAM_COLD EXT_RAM_BSS_ATTR
#else
    #define EWRAM_COLD EWRAM_DATA
#endif
```

`EXT_RAM_BSS_ATTR` is ESP-IDF's attribute for placing zero-initialized data in external
RAM; it requires `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` (set in
`sdkconfig.defaults:22`). On every other platform `EWRAM_COLD` degrades to `EWRAM_DATA`,
so upstream GBA/32X builds are unaffected — the four annotated definitions (in
`render.iwram.cpp`, `enemy.h`, `room.h`, `common.cpp`) are diff-neutral for them.

The tempting shortcut would have been one line: `#define EWRAM_DATA EXT_RAM_BSS_ATTR`,
shipping *everything* the GBA calls "slow RAM" to PSRAM. That would have been wrong,
because the GBA's constraint and ours are different:

- On the GBA, IWRAM is 32 KB. *Almost everything* must go to EWRAM, hot or not —
  `EWRAM_DATA` means "does not fit in 32 KB", not "rarely used".
- On the S3 we have ~416 KB of internal RAM. Our constraint is not fitting under 32 KB;
  it is keeping per-pixel and per-tick data off the shared, cache-mediated PSRAM path.

Concretely, these arrays are `EWRAM_DATA` upstream but are **hot** and stay internal here:

- `divTable` (`common.cpp:95`) — read on nearly every span setup; 2 KB that would
  otherwise fault cache lines against texture reads.
- `items` (`level.h:17`) — 256 objects walked by the logic tick and the renderer.
- `gVertices` / `gFaces` (`render.cpp:68-69`) — written and re-read every frame.
- even `keys` (`common.cpp:4`) is `EWRAM_DATA` upstream — a 4-byte variable the GBA
  had to evict from IWRAM.

So `EWRAM_COLD` was applied to exactly four definitions, chosen by access pattern:

| Array | Why it is safe in PSRAM |
|---|---|
| `gBackgroundCopy` (75 KB) | Written once when the inventory opens, read while it draws. A menu transition tolerates PSRAM latency; a rasterizer inner loop does not. |
| `enemiesExtra` (~25 KB) | AI navigation for at most `MAX_ENEMIES 3` enemies, a handful of cell updates per tick (`NAV_STEPS`), not per pixel. |
| `dynSectors` (24.5 KB) | Collision sectors cloned only for rooms that bridges/trapdoors modify. |
| `gSaveData` (~8 KB) | Touched at save/load time only. |

Total: ~130 KB moved out of `dram0_0_seg`, turning the 2528-byte overflow into a link
with real headroom.

## 6. Halving the framebuffer

Upstream declares the framebuffer as `uint16 fb[FRAME_WIDTH * FRAME_HEIGHT]` — two bytes
per pixel *for an 8bpp renderer*. That is GBA-VRAM legacy (`fb` there is an address into
VRAM, and mode-4 VRAM only accepts 16-bit writes) which the desktop sims inherited. At
320x240 it wastes exactly one framebuffer: 76,800 bytes. The `__ESP32__` branch right-sizes
it (`src/fixed/common.h:402-407`):

```c
#if defined(__GBA_WIN__) || defined(__ESP32_WIN__)
    extern uint16 fb[FRAME_WIDTH * FRAME_HEIGHT];
#elif defined(__ESP32__)
    // right-sized for 8bpp: W*H bytes = W*H/2 uint16 elements (saves 75KB of SRAM
    // vs the upstream 2x-oversized declaration kept on the sim for diff-minimalism)
    extern uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2];
```

The element type stays `uint16` because the rasterizer addresses the framebuffer in
2-pixel units (`VRAM_WIDTH = FRAME_WIDTH / 2` is the row stride in `uint16` elements —
a define this port had to introduce, since upstream's DOS build uses `VRAM_WIDTH`
without defining it anywhere). The Windows simulator keeps the oversized declaration on
purpose: it is the byte-exact reference build, and the smaller its diff against upstream,
the more trustworthy it stays.

Why not put `fb` in PSRAM and be done with all of this? Because the framebuffer is the
single worst possible tenant for external RAM, for three measured reasons (ESP_Sprite on
esp32.com t=13356; IDF external-RAM docs; and retro-go's architecture — ROMs in PSRAM,
display buffers in internal DMA RAM):

1. **Write-allocate:** every stray byte written to a non-resident line costs a 64 B line
   fill plus a 64 B writeback — 128 bytes of bus traffic per useful byte. A span
   rasterizer writes *everywhere*.
2. **Cache thrash:** 76.8 KB does not fit the 64 KB D-cache, so rasterizing against it
   evicts the very textures being sampled.
3. **Bus contention:** its traffic shares SPI0 with every texture read and instruction
   fetch (section 1).

## 7. The level blob lifecycle

A `.PKD` level is not a file the engine parses into its own structures — it *is* the
structures. `fmt/pkd.h` loads it by casting pointers into the blob and patching offsets
in place (`*ptr += (uint32)data`), which also means the blob gets **mutated**: tile
pointers are fixed up at load, and at runtime `animTexturesShift` rewrites texture entries
every 5 frames and camera flags are written into the data. A dirty blob cannot be
re-entered, so every level (re)start needs a pristine copy. The lifecycle, from
`src/platform/esp32/idf/main/game_main.cpp:105-133`:

```c
const void* osLoadLevel(LevelID id)
{
    // ...find the entry in the mmap'd OLVL container...

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

So the flow is: **flash mmap (pristine, read-only) -> `memcpy` to PSRAM (writable working
copy) -> engine mutates it freely -> on death/restart/next level, free and re-copy.**
Measured: TITLE.PKD (306 KB) copies in 16 ms; the largest level, LEVEL2.PKD (2.62 MB),
stays well inside the 8 MB PSRAM. This design also let us skip upstream's `ROM_READ`
flag — that flag exists to copy four mutable tables out of ROM-resident level data into
static RAM, unnecessary when the whole working copy is already writable.

The double win: texture sampling is random access, and PSRAM serves those misses 3–4x
faster than flash — while removing that traffic from the flash side of the arbiter, where
it would have collided with instruction fetch.

## 8. The one blob that stays in flash: TRACKS.AD4

The 3 MB music container is the exception, and the reason is the access pattern.
`sndPlayTrack` (`src/platform/gba/sound.cpp:199-235`) looks up an `{offset, size}` pair and
then the mixer consumes the ADPCM stream strictly linearly, ~512 bytes per 1024-sample
buffer. Sequential reads through the cache are the one thing flash does well (each 64 B
line miss prefetches the next 64 samples' worth of data, and the cost amortizes to
microseconds per mixed buffer). Copying it to PSRAM would spend 3 MB of PSRAM — nearly
half of what remains after the level — to accelerate something that is not slow. So
`TRACKS_AD4` is simply a pointer into the mmap (`game_main.cpp:233-234`):

```c
const OlvlEntry* tracks = levelsFind("TRACKS.AD4");
TRACKS_AD4 = tracks ? sLevelsBase + tracks->offset : NULL;
```

## 9. The budget as it stands

Measured on the board (COM6), after `displayInit` + `gameInit` with TITLE loaded:

| Item | Bytes | Where |
|---|---|---|
| Engine `.bss`/`.data`, hot set (section 3) | ~190 K | Internal SRAM, static |
| `fb` | 76,800 | Internal SRAM, static |
| Display ping-pong DMA buffers (2 x 30,720) | 61,440 | Internal heap |
| Game task stack | 32,768 | Internal heap |
| IDF/FreeRTOS baseline, esp_lcd, driver state | remainder | Internal heap |
| **Free internal heap after full init** | **33 KB (largest block 17 KB)** | measured |
| Cold arrays via `EWRAM_COLD` | ~134 K | PSRAM `.ext_ram.bss` |
| Level blob | up to 2.62 MB | PSRAM heap |
| TITLE.SCR placeholder (zeroed) | 76,800 | PSRAM heap |

33 KB of slack is thin but honest — and it is slack *after* everything is running at
48–63 fps. The remaining levers, in order of preference, if a future phase (audio DMA
buffers, SD card driver) needs internal RAM:

1. **Display bands 48 -> 16 lines** (`CHUNK_LINES` in `display_esplcd.c:30`): frees 40 KB
   of DMA heap for ~0.4 ms/frame extra overhead (15 transactions instead of 5). The
   comment at that line documents this as the designed fallback.
2. **Trim the game task stack** (32 KB was chosen generously, never measured for
   high-water mark).
3. **More `EWRAM_COLD` candidates** — but each new one must pass the "not touched per
   frame" test; the easy wins are already taken.
4. **`SPIRAM_RODATA` / `SPIRAM_FETCH_INSTRUCTIONS`** — moves `.rodata`/`.text` traffic
   from the flash side of the SPI0 arbiter to the PSRAM side; frees no SRAM but can buy
   bus headroom (this is the exact mechanism from esp-idf #14612).

## Key takeaways

- The S3 is not "512 KB + 8 MB of RAM": it is ~416 KB of real RAM plus 8 MB of
  cache-mediated memory that shares one bus and one 64 KB cache with flash. Placement is
  the whole game.
- The GBA port had already classified every array; the S3 port re-used that classification
  but corrected for the different constraint — `EWRAM_DATA` means "doesn't fit in 32 KB"
  on GBA, not "cold", so the mapping to PSRAM had to be a new, selective macro
  (`EWRAM_COLD`, 4 arrays, ~130 KB) instead of a wholesale redefinition.
- Hot data earns SRAM by access frequency, not size: 2 KB `divTable` stays internal while
  75 KB `gBackgroundCopy` leaves.
- The framebuffer must never live in PSRAM: write-allocate amplification (128 B of bus per
  stray byte), cache thrash (76.8 KB > 64 KB D-cache), and SPI0 contention.
- Level data is copied flash -> PSRAM per load because the engine mutates the blob in
  place and because PSRAM serves the rasterizer's random texture misses 3–4x faster;
  music stays flash-mapped because its access is sequential and cache-friendly.
- Halving `fb` (75 KB) plus `EWRAM_COLD` (~130 KB) turned a 2528-byte linker overflow
  into a running game with 33 KB of internal heap to spare — with the 48->16-line display
  band change (+40 KB) held in reserve.
