# Measured Performance and the Remaining Levers

This document is the port's performance ledger: every number actually measured on the physical
board (Waveshare ESP32-S3-Touch-LCD-2, logs over COM6), the theoretical ceilings those numbers
should be judged against, an accounting of where CPU time goes today, and the ordered backlog
of optimizations that have *not* been done yet — each with its mechanism and expected return.
The port currently runs faster than the original game shipped (48–50 fps in GYM vs the
original's 30) without a single line of the rasterizer having been optimized for this chip;
the point of this document is to record why, and what remains on the table.

Ground rules: numbers below marked **measured** came from the board or the Windows simulator.
Numbers marked *derived* are arithmetic from measured quantities or datasheet facts. Nothing
here is a guess dressed as a measurement.

## 1. The measured numbers

| Metric | Value | Status | Context |
|---|---|---|---|
| Full-frame flush, 80 MHz SPI | **16.37 ms** | measured | Phase 2 benchmark, stable across runs; wire floor is 15.36 ms → 6% overhead |
| Full-frame flush, 40 MHz SPI | ~31 ms | derived | fallback if a panel unit rejected 80 MHz (this unit did not) |
| Title screen | **62–63 fps** | measured | display-bound; slightly above the naive 61 fps ceiling — see §3 |
| GYM, in-game | **48–50 fps** | measured | real level geometry, enemies, HUD; the original game ran at 30 |
| TITLE.PKD load (306 KB), flash→PSRAM | **16 ms** | measured | ~19 MB/s effective; extrapolates to ~80 ms for GYM (1.46 MB), ~140 ms for LEVEL2 (2.62 MB) |
| Internal heap after full game init | **33 KB free** (largest block 17 KB) | measured | logged by `game_main.cpp` after `gameInit()` |
| Windows simulator (same engine, same defines) | **64 fps** | measured | i686 build; hours of soak with no crash or panic |
| PSRAM memtest at boot | pass @ 80 MHz | measured | octal DDR, 8 MB detected |

Two of these deserve their stories told in full.

### The flush number: 16.37 ms vs the 15.36 ms floor

A 320x240 RGB565 frame is 153,600 bytes = 1,228,800 bits. At 80 MHz SPI the wire itself —
ignoring every command byte, every DMA setup, every interrupt — takes 15.36 ms. That is the
physics floor for this board; the display is soldered to SPI, so no software can go below it.
Measured: 16.37 ms, i.e. **6% overhead**. The pipeline (48-line single-transaction bands,
ping-pong DMA, LUT expansion hidden under the previous band's transfer — see
04-display-pipeline.md) is effectively at the bus floor. Display-side optimization is done;
there is nothing meaningful left to win there, which is why none of the levers in §5 touch it.

### The 2,528-byte linker overflow

The first attempt to link the full engine for the board died in the linker:
`.dram0.bss` overflowed `dram0_0_seg` by **2,528 bytes**. The engine's static state — sized
for a GBA plus our 320x240 upgrades — plus the framebuffer, plus the display layer's 60 KB of
DMA buffers, plus ESP-IDF's own footprint, exceeded internal SRAM by less than a page. It was
the best possible failure: small enough to prove the fit was close, real enough to force the
memory design to be made explicit instead of accidental.

The fix was not to shave 2,528 bytes. It was the `EWRAM_COLD` macro (`src/fixed/common.h`):

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

The GBA had already classified its data for us: anything upstream marked for slow EWRAM is,
by construction, data the engine can afford to access slowly. Mapping that same judgment onto
PSRAM (via `EXT_RAM_BSS_ATTR`, enabled by `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`)
moved the four large cold arrays out of internal SRAM:

| Array | Size | Why it's cold |
|---|---|---|
| `gBackgroundCopy` (render.cpp:67) | 75 KB | touched only when the inventory opens |
| `enemiesExtra` (enemy.h:12) | ~25 KB | per-enemy AI extras, low-rate access |
| `dynSectors` (room.h:10) | 24 KB (3072 × 8 B `Sector`) | dynamic sector shadow, sparse writes |
| `gSaveData` (common.cpp:18) | 8 KB | savegame staging only |

~130 KB freed. Hot data stayed internal on purpose: `fb` (76.8 KB, right-sized to
`uint16 fb[FRAME_WIDTH * FRAME_HEIGHT / 2]` on `__ESP32__`, saving 75 KB vs the upstream
2x-oversized GBA-VRAM-legacy declaration), `gVertices` (40 KB), `gFaces` (30 KB), `items`
(19.5 KB), `gLightmap` (8 KB, read per textured pixel), `divTable` (2 KB, read per span).
The remaining margin is the measured **33 KB** — thin but stable, with a pre-negotiated
+40 KB escape hatch (display bands 48→16 lines, costing ~0.4 ms/frame). The full layout
rationale lives in 05-memory-map.md.

## 2. Theoretical ceilings

What the display bus permits, independent of the engine:

| SPI clock | Bytes/frame | Wire time | FPS ceiling (wire) | Measured flush | Practical ceiling |
|---|---|---|---|---|---|
| 40 MHz | 153,600 | 30.72 ms | 32.6 | ~31 ms (derived) | ~32 |
| **80 MHz** | 153,600 | **15.36 ms** | **65.1** | **16.37 ms** | **~61 (serialized)** |

Both ceilings are far above the original game's 30 fps target, which is the real budget: the
engine's logic runs at 30 Hz ticks (`gameUpdate` consumes 33 ms units) regardless of render
rate. 80 MHz is what every vendor demo for this board ships, and this unit runs it clean; the
40 MHz row exists as the documented fallback and as context for how much the clock doubling
bought (the difference between "display-bound at 32" and "engine-bound at 48–50").

## 3. CPU accounting: what actually runs during a frame

The game loop (`game_main.cpp`, single task pinned to core 0) is strictly serial:
`inputUpdate → gameUpdate → gameRender → displayFlush`, then a 1-tick `vTaskDelay`. Core 1 is
idle by design, reserved for Phase 4b/5. Within `displayFlush`, per 48-line band:

- **CPU work:** ~0.3 ms of LUT expansion (76,800 px/frame at ~4–5 cycles/px ≈ 1.4–1.5 ms
  total), plus microseconds of semaphore/queue traffic and one short IRAM ISR per band.
- **DMA work:** ~3.07 ms of wire time per band, ~0% CPU.
- **Blocked time:** everything else. The take-before-write protocol blocks the game task
  whenever expansion gets ahead of the wire — which is most of the flush.

Because `displayFlush` returns after *queueing* the last band (see 04-display-pipeline.md §5),
the timeline of a frame looks like this (*derived* from the measured band times, not
independently measured):

```
game task:  [update+render][exp][exp]--wait--[exp]--wait--[exp]--wait--[exp]| next update+render
wire (DMA):               [band0 ][band1 ][band2 ][band3 ][band4 ]
                          ^ flush call                    ^ flush returns (~10 ms in),
                                                            bands 3-4 still on the wire (~6 ms)
```

Those trailing ~6 ms of wire time overlap the next frame's update+render for free. This
overlap — plus integer-per-second fps counting — is how the title screen measures 62–63 fps
against a naive serialized ceiling of 1/16.37 ms ≈ 61.

The accounting for GYM at 48–50 fps (20–21 ms/frame) therefore splits roughly into: ~10 ms of
flush wall-time inside the game task (of which only ~1.5 ms is CPU), and ~10 ms of
update+render, partially overlapped with the trailing bands. Read that carefully: **the game
task spends ~8.5 ms per frame doing nothing but waiting for a wire that DMA is already
driving.** That is the dual-core lever in §5, and it is the largest single item on the table.

## 4. The measure-first rule

Every lever below is ranked by *expected* return, and none gets applied without a before/after
measurement. The instrumentation already exists upstream — `src/fixed/common.h` opens with it:

```c
//#define PROFILING
#ifdef PROFILING
    #define STATIC_ITEMS
    #define PROFILE_FRAMETIME
//    #define PROFILE_SOUNDTIME
#endif
```

`PROFILE_FRAMETIME` splits the frame into named counters — `CNT_UPDATE`, `CNT_RENDER`, and
stage counters `CNT_TRANSFORM`/`CNT_ADD`/`CNT_FLUSH`/`CNT_VERT`/`CNT_POLY`
(common.h:2964–2999). `STATIC_ITEMS` freezes item logic so successive frames are comparable.
Wiring the counters to `esp_timer_get_time()` on the ESP32 side is a small, not-yet-done
platform task; it is the *first* item of Phase 5, before any optimization, because a lever
applied without a profile is a superstition with a commit hash.

## 5. The ordered backlog

### 5.1 `IRAM_ATTR` on the rasterizer and hot render path

**Mechanism.** All code currently executes from flash through the 32 KB I-cache. The
rasterizer's inner loops (`rasterize*` in `src/platform/esp32/rasterizer.h`, plus the hot
paths of `render.cpp`) share that cache with the entire engine; every I-cache miss during a
span loop stalls the pipeline for a flash line fill — and competes on the shared SPI0 bus
with PSRAM texture reads (flash and PSRAM sit behind one arbiter; Espressif engineer,
esp-idf issue #14612). Moving the inner loops to IRAM removes both the stalls and their bus
traffic. This is the direct analog of what the GBA build does with IWRAM code (the renderer
file is literally named `render.iwram.cpp`), and of why the display ISR is already
`IRAM_ATTR`.

**Cost/risk.** IRAM is a scarce, fixed pool shared with the ISRs and parts of IDF; the
rasterizer must be moved selectively (inner loops, not the whole translation unit).
**Expected return:** moderate and scene-dependent; unmeasured — this is the first thing
`PROFILE_FRAMETIME` will quantify via `CNT_FLUSH`/`CNT_POLY` deltas.

### 5.2 Dual-core split

**Mechanism.** Move palette expansion + `displayFlush` (and, once Phase 4b lands, audio
mixing) to a task on core 1. The game task hands off the finished `fb` and immediately starts
the next `gameUpdate`/`gameRender`; the ~10 ms of per-frame flush wall-time (§3) disappears
from the critical path entirely. The 3-function display interface makes this a contained
change: `displayFlush` becomes "signal core 1", and the take-before-write semaphore protocol
already makes buffer ownership explicit. One new hazard appears: the engine must not begin
rasterizing frame N+1 into `fb` while core 1 is still expanding frame N out of it — either a
second framebuffer (76.8 KB — currently unaffordable internally, see the 33 KB margin) or a
band-granular handshake (expand band k before the rasterizer may touch band k again) is
required; the handshake variant costs no RAM.

**Expected return:** the largest on the list. Removing ~10 ms of blocked time from a 20 ms
frame puts GYM at the display ceiling (~60 fps) if update+render fits in 16 ms — which at
~10 ms today it already does. The plan's original estimate for this class of change (+30%)
now looks conservative against the measured split.

### 5.3 Potato profile relaxation (spending the surplus on quality)

The engine currently ships the proven GBA cut-down profile, adopted wholesale
(`src/fixed/common.h`):

```c
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

This lever runs *backwards*: it spends fps headroom to buy back visual and gameplay fidelity.
What each flag does (comments per the upstream GBA block, common.h:240–253):

| Flag | Effect while defined | Cost of relaxing it |
|---|---|---|
| `HIDE_CORPSES (30*10)` | dead enemies vanish after 10 s (300 ticks at 30 Hz) to cut polygons | more corpses on screen → more polys in crowded fights |
| `LOD_TRAP_FLOOR` | collapsing-floor traps render as two flat quads when static | full trap geometry, more polys per trap room |
| `NO_STATIC_MESH_PLANTS` | plant static meshes removed | restores foliage → transparent overdraw, the most expensive kind |
| `MAX_ENEMIES 3` | at most 3 concurrently active enemies (upstream default 8) | AI + animation + render cost per extra enemy; **also grows `enemiesExtra`**, which is `EWRAM_COLD` (PSRAM), so the RAM cost lands harmlessly |
| `VIEW_DIST (10 << 10)` | visibility clipped at 10,240 units (10 world blocks) | more rooms/portals traversed and drawn per frame — likely the most visible upgrade and the most expensive |
| `FAST_HITMASK` | skips `collideSpheres` for enemies (coarse hit detection) | exact per-sphere hit tests; pure CPU, gameplay-fidelity win |

The rule inherited from the plan: relax **one flag at a time**, measure GYM (not the title
screen) before and after, keep what stays above 30 fps with margin. The natural order is
cheapest-first: `FAST_HITMASK`, `HIDE_CORPSES`, `LOD_TRAP_FLOOR`, `MAX_ENEMIES`, then the two
overdraw/traversal hitters `NO_STATIC_MESH_PLANTS` and `VIEW_DIST`.

### 5.4 The 120 MHz PSRAM experiment

**Mechanism.** `sdkconfig.defaults` currently pins `CONFIG_SPIRAM_SPEED_80M`. IDF offers an
experimental 120 MHz octal DDR mode. Published measurements (elect-gombe's Qiita benchmark):
124.8 MB/s sequential at 120 MHz vs ~84 MB/s at 80 MHz (+49%); random reads over a 4 MB span
still only 8.2 MB/s (~0.5 µs per 64-byte cache-line miss — latency, not bandwidth, governs
random access). Since level data and textures live in PSRAM, texture-fetch-heavy scenes gain;
LUT expansion and the framebuffer do not (both internal by design — see 05-memory-map.md for
why the framebuffer must never live in PSRAM).

**Risk.** The mode is documented as experimental and temperature-sensitive; it was
deliberately excluded from the baseline config. Run it as an A/B with `PROFILE_FRAMETIME`,
on a warm board, and keep it only if the render-stage counters move outside noise.

### 5.5 `SPIRAM_RODATA` (and `SPIRAM_FETCH_INSTRUCTIONS`)

**Mechanism.** Flash and PSRAM share the SPI0 bus and the D-cache behind an arbiter. Today,
`.rodata` (and all code) is served from flash; every miss competes with PSRAM texture traffic.
`CONFIG_SPIRAM_RODATA` copies `.rodata` to PSRAM at boot (and `SPIRAM_FETCH_INSTRUCTIONS`
does the same for `.text`), consolidating traffic on the faster device: an octal-PSRAM miss
costs ~0.5 µs vs ~1.8–2 µs for quad flash. Precedent: in esp-idf issue #14612 an Espressif
engineer's case recovered a 4→16 MHz effective LCD pixel clock just by moving assets off the
flash side of the arbiter.

**Cost.** PSRAM capacity (abundant: level blob peaks at 2.62 MB of 8 MB) and boot time. It is
a one-line config experiment — cheap enough that its position this far down the list is only
because its winnings overlap with 5.1 (both attack flash-side contention; do 5.1 first, then
check whether this still moves anything).

### 5.6 Texture staging / swizzle

**Mechanism.** If, after all the above, `PROFILE_FRAMETIME` shows textured-span time dominated
by PSRAM latency (random texel fetches at ~0.5 µs/line, 8.2 MB/s effective), two classic
fixes apply: stage the active object's texture/mip into an internal-SRAM scratch before its
spans are drawn, or swizzle texture tiles at level-load time so that neighboring texels share
a 64-byte cache line (the S3's line size, `CONFIG_ESP32S3_DATA_CACHE_LINE_64B` — itself
mandatory-safe with octal DDR PSRAM, whose 64-byte wrap bursts corrupt a 32-byte-line cache
per arduino-esp32 issue #12480).

**Why last.** It is the only lever that touches engine data layout, it needs the 33 KB margin
renegotiated (or the 16-line-band lever pulled), and there is no evidence yet that texel
latency is the bottleneck — the 48–50 fps measurement was taken with textures already in
PSRAM and nothing staged. This is the definition of a measure-first item.

## Key takeaways

- Everything measured, nothing regressed: 16.37 ms flush (6% over the physical wire floor),
  62–63 fps title, 48–50 fps in GYM vs the original's 30, 16 ms level load, 64 fps simulator
  parity, 33 KB of internal heap to spare.
- The display side is finished: at 6% over the bus floor there is nothing left to optimize —
  every remaining lever targets the engine side or the memory system.
- The 2,528-byte linker overflow was the cheapest possible warning shot; `EWRAM_COLD` turned
  the GBA's own hot/cold data classification into a PSRAM placement policy and freed ~130 KB.
- The single biggest unclaimed win is structural, not micro: the game task wastes ~10 ms per
  frame blocked on a DMA it doesn't need to watch. A core-1 flush task converts GYM from
  48–50 fps to display-bound.
- The potato profile is a quality budget, not a performance one — six flags, each with a
  known price, to be relaxed one at a time against measured GYM framerates.
- 120 MHz PSRAM, `SPIRAM_RODATA`, and texture staging are all real levers with published
  evidence behind their mechanisms — and all gated behind the same rule: `PROFILE_FRAMETIME`
  exists upstream; wire it up first. No optimization without a before/after number.
