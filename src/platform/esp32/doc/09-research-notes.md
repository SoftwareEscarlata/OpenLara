# Research Findings That Shaped the Port

Three questions had to be answered with evidence, not intuition, before the port's
architecture froze: where level data should live (PSRAM vs. flash), whether a second
ESP32 could act as a GPU, and whether a display framework (LovyanGFX) would beat a
hand-rolled `esp_lcd` pipeline. This document records each investigation — the sources,
the measured numbers, and the verdicts — so that future changes argue against the data
instead of re-litigating folklore. Each verdict points at the code that implements it.

## Investigation A: PSRAM vs. flash for random texture reads

### The question

OpenLara's fixed-point engine reads level data (geometry, and above all **textures**)
with an essentially random access pattern during rasterization. On the GBA this data
streams straight from cartridge ROM. The S3 equivalent would be `esp_partition_mmap()`
and reading from flash through the cache. Is that good enough, or should the ~2.6 MB
level blob be copied into the 8 MB octal PSRAM at load time?

### Fact 1: flash and PSRAM share one bus and one cache

An Espressif engineer, in esp-idf issue #14612, confirms the part of the S3 memory
architecture that matters most here: **external flash and PSRAM share the SPI0 bus and
the same data cache, with an arbiter between them**. Every instruction-cache miss (code
runs from flash) and every data miss (textures) queue up on the same physical bus. In
that same issue, moving assets from flash to PSRAM (`CONFIG_SPIRAM_RODATA`) recovered an
effective LCD pixel clock from 4 MHz to 16 MHz — a 4x system-level win from *relieving
the flash side of the shared bus*, not from any single access getting faster.

- Source: https://github.com/espressif/esp-idf/issues/14612

### Fact 2: the measured numbers

| Measurement | Value | Source |
|---|---|---|
| Octal PSRAM, 80 MHz DDR, sequential read | ~84 MB/s | esp32.com t=29970 |
| Octal PSRAM, 120 MHz DDR, sequential | 124.8 MB/s | elect-gombe Qiita benchmark |
| Octal PSRAM, 120 MHz DDR, random over a 4 MB span | **8.2 MB/s** | same |
| Cost of one random cache-line miss from PSRAM | ~0.5 us | same |
| 64 B cache line vs. 32 B, sequential throughput | **+49%** | same |
| Quad flash, one cache-line miss | ~1.8-2 us | same family of measurements |
| Quad flash, API reads | 8.7-11.4 MB/s | mculab N16R8 bench (URL not recorded) |
| `memcpy` DRAM->DRAM vs. PSRAM->PSRAM | 358.7 vs. 34.8 MB/s | esp32.com t=25215 |

Sources: https://esp32.com/viewtopic.php?t=29970 ·
https://esp32.com/viewtopic.php?t=25215 · Qiita benchmark by user *elect-gombe*
(qiita.com/elect-gombe; the exact article URL was not recorded in the session notes).

The decisive line is the miss cost: a random texture fetch that misses cache is served
in **~0.5 us from octal PSRAM vs. ~1.8-2 us from quad flash — PSRAM is 3-4x faster per
miss**, *and* it keeps texture traffic off the flash half of the arbiter so instruction
fetches don't stall behind it. The 8.2 MB/s random figure also calibrates expectations:
random access is ~15x worse than sequential even in PSRAM, which is why hot engine
state stays in internal SRAM (see 05-memory-map.md).

### Fact 3: the 64-byte cache line is required, not a tuning knob

Octal DDR PSRAM transfers in **64-byte wrap bursts**. Configuring the S3's data cache
with 32-byte lines against octal PSRAM is documented to **corrupt the cache**
(arduino-esp32 issue #12480). So in `sdkconfig.defaults`:

```
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_8WAYS=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

`LINE_64B` is *required-safe* — correctness first, and the +49% sequential throughput is
the bonus. None of these are IDF defaults.

- Source: https://github.com/espressif/arduino-esp32/issues/12480

### Fact 4: the framebuffer must never live in PSRAM

Three independent reasons, each fatal on its own:

1. **Write-allocate amplification.** The cache fills a 64-byte line before a write and
   writes 64 bytes back — a single stray framebuffer byte costs up to 128 bytes of bus
   traffic.
2. **Thrash.** The 76.8 KB framebuffer is larger than the entire 64 KB D-cache;
   rasterizing into a PSRAM framebuffer would continuously evict the textures the same
   inner loop is trying to read.
3. **Bus contention.** Framebuffer writes would fight texture reads on the same shared
   SPI0 bus (see Fact 1).

Corroboration: ESP_Sprite (Espressif) on framebuffers in external RAM,
https://esp32.com/viewtopic.php?t=13356; the ESP-IDF external-RAM guide,
https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html;
and the **retro-go** precedent (https://github.com/ducalex/retro-go) — the most mature
ESP32 retro-gaming firmware — which puts ROMs in PSRAM but display buffers strictly in
internal `MALLOC_CAP_DMA` RAM. **esp32-doom**
(https://github.com/espressif/esp32-doom) draws the same line from the other side: WAD
data external, working buffers internal.

### The design this produced

`osLoadLevel()` in `src/platform/esp32/idf/main/game_main.cpp`:

```c
// the engine mutates the level image (tile fixups, animated textures) and
// needs a pristine copy per (re)load — free the old one, copy from flash.
// PSRAM serves random texture reads 3-4x faster than flash and keeps the
// flash side of the shared SPI0 bus free for code fetch.
free(levelData);
levelData = (uint8*)heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM);
...
memcpy(levelData, sLevelsBase + e->offset, e->size);
```

- Levels: flash-mmap'd partition as *source only*; active level memcpy'd to PSRAM at
  load. Measured on hardware: TITLE.PKD (306 KB) copies in **16 ms** — the largest
  level (~2.62 MB) extrapolates to well under a loading screen.
- The copy being writable is also *required*: the engine patches the level image in
  place at load and at runtime, so the GBA's `ROM_READ` flag stays off.
- Music (`TRACKS.AD4`, 3 MB) stays flash-mmap'd: the mixer reads it sequentially, and
  sequential flash through the cache is cheap — only random access is expensive.
- Framebuffer and hot engine state: internal SRAM, always (05-memory-map.md).
- End result on hardware: 48-63 FPS with the display, engine, and PSRAM all sharing
  those buses.

## Investigation B: a second ESP32 as GPU

### The survey

The idea recurs in every MCU-3D thread: dedicate a second chip to graphics. The prior
art actually examined:

- **MicroGPU** (https://github.com/KallDrexx/microgpu) — an ESP32-S3 serving as a 2D
  "GPU" for slow hosts (its real target was a ~72 MHz Meadow F7).
- **FabGL** (https://github.com/fdivitto/FabGL) — ESP32 as a VGA terminal/graphics
  processor for a host machine.
- **EVE FT81x** family (Bridgetek, https://brtchip.com) — the commercial embodiment of
  the same architecture.

All three share one structural condition of success: **the host sends compact draw
calls and the graphics chip expands them into pixels**. "Fill this rectangle" is ~50
bytes that becomes 20,000 pixels — the link carries commands, never pixels.

### The bandwidth math that kills it

OpenLara breaks that condition structurally: the fixed-point engine does not emit draw
calls. Its output *is* a framebuffer — 76,800 bytes of palette-indexed pixels produced
by an affine-textured span rasterizer with palette lighting and painter's sorting. No
MicroGPU/FabGL/EVE primitive set expresses that without rewriting the renderer.

So the inter-chip link would have to carry pixels or triangles, and the numbers close
every door:

| Path | Bandwidth needed | What the link provides |
|---|---|---|
| Ship 8bpp framebuffer @ 30 FPS | ~2.3 MB/s | ESP32-S3 as SPI slave sustains **2-5 MB/s** real |
| Ship RGB565 @ 30 FPS | ~4.6 MB/s | over the slave-link budget |
| Ship triangles: ~2-3k visible x ~30 B x 30 FPS | ~2-2.7 MB/s | same order as just shipping the frame |
| Today's display link (SPI master, 80 MHz, DMA) | — | ~10 MB/s at ~0% CPU |

Reading the table: the chip-to-chip link is *equal to or worse than* the display bus it
was supposed to offload, while adding a frame of latency, a protocol, vsync
synchronization, a second firmware, and a second BOM line. Offloading rasterization
fares no better: the receiving chip is the *same* 240 MHz Xtensa with no raster
hardware, so total work doesn't shrink — it moves, plus serialization overhead. The only
traffic-reducing variant (send 8bpp, let the far chip expand the palette) saves 2x on a
link we didn't need, to do a 2-3 ms job that core 1 of the S3 can do for free while the
DMA runs.

### The verdict

Rejected. Even MicroGPU's own author reached the matching conclusion — that the S3 is
the wrong silicon for the GPU role — and pivoted toward FPGA / ESP32-P4
(https://github.com/KallDrexx/microgpu/issues/22). Everything that actually moves the
needle lives *inside* one S3: framebuffer in internal SRAM, textures in octal PSRAM,
palette expansion overlapped with DMA, both cores used. If more silicon is ever truly
wanted, the honest upgrade is a single ESP32-P4 (400 MHz dual RISC-V, PPA blitter,
MIPI-DSI), not a two-chip split.

## Investigation C: LovyanGFX vs. raw `esp_lcd`

### The claim to check

Community advice (a Reddit thread; URL not recorded in the session notes) held that
LovyanGFX (https://github.com/lovyan03/LovyanGFX) is "12% faster than TFT_eSPI" and the
obvious choice, and its 8bpp palette sprites (`setColorDepth(8)` + `createPalette()`)
map 1:1 onto OpenLara's indexed framebuffer + `osSetPalette()`.

### What reading the source showed

The port's display need is exactly one operation: blit a full 8bpp indexed frame
through a 256-entry palette to the panel, every frame. Following that path through
LovyanGFX's source: `LGFX_Sprite::pushSprite()` routes through the `pixelcopy_t`
machinery (`src/lgfx/v1/misc/pixelcopy.hpp`), whose palette conversion
(`copy_palette`-style converters) expands indexed pixels to RGB565 **on the CPU,
through a generic function-pointer-per-conversion path**. There is no DMA-fed hardware
palette; the expansion work is identical in kind to our own loop, just filtered through
a framework generic enough to convert any format to any format.

Meaning: LovyanGFX would give us the *same* CPU palette expansion, slower (indirect
calls, generality) than a dedicated loop, plus a C++ framework wrapped around a port
that needs three functions.

### Why the 12% claim measures the wrong thing

The benchmark behind it times drawing primitives — `drawLine`, `fillRect`, circles —
that OpenLara **never calls**. A software-rasterizer port does not draw lines on the
display; it produces a finished framebuffer and needs one massive blit per frame. A
framework's primitive speed is irrelevant to a workload whose display cost is pure
bulk transfer plus palette expansion.

### What won instead

Raw `esp_lcd` behind the 3-function interface of `idf/main/display.h`
(`displayInit` / `displaySetPalette` / `displayFlush`), with the expansion loop we
control (`display_esplcd.c`):

```c
static inline void expandChunk(const uint8_t* restrict src, uint16_t* restrict dst, int pixels)
{
    // 2 pixels per 32-bit store, ~4-5 cycles/pixel on the LX7; the 512B LUT
    // stays cache-resident. A 48-line band (~0.3 ms) hides entirely inside
    // the previous band's ~3 ms SPI transfer.
    uint32_t* restrict d = (uint32_t*)dst;
    for (int i = 0; i < pixels; i += 2) {
        *d++ = (uint32_t)sLut[src[i]] | ((uint32_t)sLut[src[i + 1]] << 16);
    }
}
```

The LUT is pre-byteswapped at palette-set time (`sLut[i] = (rgb565 >> 8) | (rgb565 << 8)`)
because `esp_lcd` does **not** swap SPI color bytes — ESP-IDF's own LCD example calls
`lv_draw_sw_rgb565_swap()` for the same reason. Doing the swap once per palette change
instead of once per pixel is exactly the kind of decision a framework path would have
made for us, per pixel.

Measured result of the hand-rolled pipeline: 16.37 ms full-frame flush at 80 MHz — 6%
over the theoretical wire floor. There was no meaningful room left for any framework to
claim.

## Key takeaways

- **Copy levels flash->PSRAM at load.** A random miss costs ~0.5 us from octal PSRAM
  vs. ~1.8-2 us from flash (3-4x), and moving texture traffic off the flash side of the
  shared SPI0 bus/arbiter also un-stalls instruction fetch (esp-idf #14612).
- **`DATA_CACHE_LINE_64B` is a correctness flag** with octal DDR PSRAM (64-byte wrap
  bursts; 32 B lines corrupt the cache — arduino-esp32 #12480). +49% throughput is the
  side effect.
- **The framebuffer never goes to PSRAM**: write-allocate amplification, D-cache thrash
  (76.8 KB > 64 KB), and bus contention. retro-go and esp32-doom both draw the same
  boundary.
- **Sequential flash is fine**: `TRACKS.AD4` stays mmap'd; only random access is
  expensive.
- **A second ESP32 as GPU fails on structure and on arithmetic**: OpenLara outputs
  pixels, not draw calls; an S3 SPI slave link (2-5 MB/s) is no better than the display
  bus it would offload; the offload target is an identical CPU. Successful GPU-chip
  designs (MicroGPU, FabGL, EVE) all ship compact commands instead.
- **Frameworks were rejected on evidence, not taste**: LovyanGFX's own source shows 8bpp
  palette sprites expand on the CPU through generic function-pointer paths; the cited
  12% benchmark measures primitives this port never calls. Raw `esp_lcd` plus a
  dedicated LUT loop landed 6% off the wire-speed floor.
- Hardware context for all of this is in 02-hardware.md; where each byte lives is in
  05-memory-map.md.
