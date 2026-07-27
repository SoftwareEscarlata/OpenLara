# The Display Pipeline: 8bpp to ST7789 at the Bus Floor

The engine ends every frame with 76,800 bytes of palette indices in internal SRAM. The panel
wants 153,600 bytes of RGB565 over a SPI wire. This document explains every design decision
between those two points: the deliberately tiny display interface, the palette LUT and why its
entries are byte-swapped, how the band size was derived from a hardware register limit, the
ping-pong/semaphore protocol that makes DMA safe, what the `esp_lcd` driver does for us (and
what it pointedly does not), and the two board-level constraints — backlight sequencing and the
shared SD bus — that shaped the init sequence. After reading it you should be able to predict
the measured 16.37 ms flush time from first principles.

All code quoted below is real and current, from `src/platform/esp32/idf/main/display_esplcd.c`,
`display.h`, and `board_pins.h`. Measured numbers come from the physical board (see
08-performance.md for the full performance picture).

## 1. The interface: three functions and a drain

`src/platform/esp32/idf/main/display.h` is the entire contract between the game and the screen:

```c
// Display layer for the OpenLara ESP32-S3 port.
// Three functions hide the backend (esp_lcd native / LovyanGFX) so swapping
// implementations is a build-system change, not a code change.

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

// Init bus + panel + backlight. Returns 0 on success.
int displayInit(void);

// 256-entry palette in OpenLara's BGR555 format (bits 0-4 = R, 5-9 = G, 10-14 = B).
// Converted internally to pre-byteswapped RGB565 for the panel.
void displaySetPalette(const uint16_t* pal256);

// Push a full 8bpp indexed frame (DISPLAY_WIDTH*DISPLAY_HEIGHT bytes).
void displayFlush(const uint8_t* fb8);

// Blocks until the last displayFlush transfer fully completed.
void displayWaitFlush(void);
```

Why so small? Because during planning there were two candidate backends: raw `esp_lcd` (IDF's
native panel API) and LovyanGFX (a mature Arduino/IDF graphics framework with ST7789 support).
The interface was designed so that choosing between them would be a build-system decision — a
different `.c/.cpp` file implementing the same four symbols — not a code change in the engine
or the game harness. `game_main.cpp` calls exactly these functions and nothing else
(`osSetPalette` is a one-line forward to `displaySetPalette`).

The evaluation then killed LovyanGFX on its merits: reading its source showed that its 8bpp
paletted sprite path (`LGFX_Sprite::pushSprite` → `pixelcopy_t::copy_palette`) expands
palette indices to RGB565 **on the CPU anyway**, through a generic function-pointer copy
routine — strictly slower than a dedicated LUT loop, plus a whole framework of primitives
(`drawLine`, `fillRect`) that OpenLara never calls, which is exactly what the oft-quoted
"12% faster than TFT_eSPI" benchmark measures. There is no hardware path a framework could
unlock here: the ST7789 accepts pixels, and someone has to expand them. So the shipped backend
is `display_esplcd.c`, and the interface remains as insurance.

Note the fourth function: `displayWaitFlush` exists because `displayFlush` is deliberately
asynchronous (it returns with up to two bands still in flight). Anyone who needs the wire
quiet — the init code before switching the backlight on, or a future SD card access — drains
the pipeline explicitly.

## 2. The data path

```
  fb (uint16[320*240/2], 8bpp indices, internal SRAM, 76,800 B)
       |
       |  expandChunk(): 48 lines at a time through sLut[256]
       |  ~0.3 ms CPU per band, 2 pixels per 32-bit store
       v
  sChunk[0] / sChunk[1]  (ping-pong, 2 x 30,720 B,
       |                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)
       |  esp_lcd_panel_draw_bitmap() — by reference, zero copy
       v
  SPI2 @ 80 MHz, DMA  ——  one transaction per band, ~3.07 ms on the wire
       |                  on_color_trans_done ISR returns the buffer
       v
  ST7789T3, 240x320 portrait rotated to 320x240 by MADCTL (swap_xy + mirror)
```

Five bands per frame (240 / 48). While band N is on the wire, the CPU expands band N+1 into
the other buffer; the ~0.3 ms of LUT expansion hides entirely inside the previous band's
~3 ms transfer. The steady-state cost of a frame is therefore the wire time plus small
per-band gaps: 15.36 ms floor, 16.37 ms measured (6% overhead).

The core loop, verbatim from `display_esplcd.c`:

```c
void displayFlush(const uint8_t* fb8)
{
    int buf = 0;
    for (int y = 0; y < DISPLAY_HEIGHT; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES <= DISPLAY_HEIGHT) ? CHUNK_LINES : (DISPLAY_HEIGHT - y);

        // acquire a free buffer BEFORE writing into it
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        expandChunk(fb8 + y * DISPLAY_WIDTH, sChunk[buf], lines * DISPLAY_WIDTH);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, DISPLAY_WIDTH, y + lines, sChunk[buf]);

        buf ^= 1;
    }
}
```

## 3. The LUT: BGR555 in, byte-swapped RGB565 out

OpenLara's fixed engine is a GBA engine at heart, and its palette format is the GBA's:
15-bit BGR555 with **red in the low bits** (bits 0–4 = R, 5–9 = G, 10–14 = B). The panel wants
RGB565. Rather than convert per pixel, `displaySetPalette` converts the 256 palette entries
once per palette change (the engine calls `osSetPalette` on load and on every fade tick):

```c
void displaySetPalette(const uint16_t* pal256)
{
    // OpenLara BGR555 (R low) -> RGB565, byteswapped for the panel
    for (int i = 0; i < 256; i++) {
        uint16_t c = pal256[i];
        uint16_t r = (c & 0x1F);
        uint16_t g = (c >> 5) & 0x1F;
        uint16_t b = (c >> 10) & 0x1F;
        uint16_t rgb565 = (r << 11) | (g << 6) | b;   // 5-6-5, G gets the extra bit
        sLut[i] = (rgb565 >> 8) | (rgb565 << 8);      // SPI sends MSB first
    }
}
```

Two details deserve unpacking:

**The green shift.** RGB565 gives green 6 bits (bits 5–10). Our source green is 5 bits, so
`g << 6` places it in the *top* five bits of the green field and leaves the low bit zero —
the standard 5-to-6-bit promotion without the replication refinement (imperceptible at this
palette depth).

**The byte swap — why it exists at all.** The ST7789 expects each 16-bit color with its most
significant byte first on the wire. The S3's SPI peripheral transmits bytes from memory in
memory order, and the Xtensa is little-endian, so a `uint16_t` `0xAABB` in RAM goes out as
`BB AA` — backwards. Crucially, **`esp_lcd` does not swap color bytes for you**: IDF's own
LVGL example calls `lv_draw_sw_rgb565_swap()` on every buffer before handing it to
`draw_bitmap`, i.e. upstream pays a per-frame, per-pixel pass just to swap bytes. We pay it
**never**: the swap is baked into the 256 LUT entries at palette-set time, so the expansion
loop emits wire-ready bytes for free. This is the single biggest advantage of an indexed-color
source: any per-pixel transform you can express per palette entry costs 256 operations, not
76,800.

The expansion loop itself:

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

Two LUT reads, one shift, one OR, one 32-bit store per pixel pair. The 512-byte LUT lives
permanently in D-cache. At ~4–5 cycles/pixel and 240 MHz, a 15,360-pixel band costs ~0.3 ms —
a tenth of the band's wire time, which is why the pipeline is wire-bound, not CPU-bound.

## 4. Band sizing: why exactly 48 lines

```c
// lines per DMA chunk: 48 lines * 320 px * 2 B = 30720 B — just under the S3's
// 32768-byte-per-transaction hardware cap, so each band is exactly ONE SPI
// transaction (5 per frame, ~0.2 ms total overhead). x2 ping-pong = 60 KB DMA RAM.
// If SRAM gets tight in later phases, 16-line bands cost only ~0.4 ms more.
#define CHUNK_LINES 48
#define CHUNK_BYTES (DISPLAY_WIDTH * CHUNK_LINES * 2)
```

The governing constant is a hardware register width: the S3's GP-SPI transaction length field
is 18 bits of *bits* (`SPI_LL_DMA_MAX_BIT_LEN` = 2^18 bits = 262,144 bits = **32,768 bytes
per transaction**). Any transfer longer than that must be split into multiple transactions,
each paying command/CS/DC framing, DMA descriptor setup, and an interrupt.

So the sizing question becomes: what is the largest band that (a) fits in one transaction and
(b) divides 240 evenly so every band is the same size?

| Lines/band | Bytes | Fits 32,768? | Divides 240? |
|---|---|---|---|
| 60 | 38,400 | no | yes |
| 48 | **30,720** | **yes** | **yes (5 bands)** |
| 40 | 25,600 | yes | yes (6 bands) |
| 16 | 10,240 | yes | yes (15 bands) |

48 is the largest divisor of 240 under the cap. Result: 5 transactions per frame, keeping
total transaction/framing overhead around 0.2 ms. The measured end-to-end overhead is
16.37 − 15.36 = ~1.0 ms (6%), the remainder being inter-band scheduling gaps (semaphore
wakeup, `draw_bitmap` command phases between color bursts).

Cost side: two ping-pong buffers of 30,720 B = **60 KB of internal, DMA-capable SRAM** — the
display layer's entire RAM bill besides the 512 B LUT. The comment's escape hatch is real and
pre-negotiated: if a later phase needs SRAM back, dropping to 16-line bands frees 40 KB and
costs only ~0.4 ms/frame of extra transaction overhead (15 transactions instead of 5). That
trade is listed as a standing memory lever in 05-memory-map.md.

### The vendor `max_transfer_sz=4000` trap

This sizing is not hypothetical hygiene. Waveshare's own ESP-IDF demos for this exact board
configure the SPI bus with `max_transfer_sz = 4000`. `esp_lcd` silently honors that limit by
slicing every color buffer: a full 153,600-byte frame becomes **39 transactions instead of 5**
— roughly 8x the per-transaction overhead — with nothing on the panel or bus requiring it. Any
benchmark of "SPI display speed on this board" built on the vendor demo starts ~8x
overhead-handicapped. Our bus config ties the limit to the band size, so the driver never
splits:

```c
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,   // for the SD card; LCD IO stays write-only
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_BYTES,
    };
```

## 5. The ping-pong protocol: a counting semaphore that counts buffers

The hazard in any DMA pipeline is writing into a buffer the DMA engine is still reading. With
two buffers and asynchronous completion, the failure mode looks like this: `displayFlush`
expands band 0 into buffer A, queues it, expands band 1 into buffer B, queues it — and now
wants buffer A for band 2. If band 0's transfer has not finished, writing band 2's pixels into
A corrupts the tail of band 0 *on screen* (the classic shearing/garbage-band artifact), or
worse, the DMA engine reads a half-rewritten descriptor region. The bug is timing-dependent:
it only appears when expansion outruns the wire, which it always does here (0.3 ms vs 3 ms —
the CPU reaches buffer A again just 0.6 ms into A's 3 ms transfer).

The protocol that prevents it is three lines of FreeRTOS, chosen so that the invariant is
structural rather than checked:

```c
    // counts FREE ping-pong buffers: take one before writing into a buffer,
    // each on_color_trans_done returns one. Guarantees we never overwrite a
    // buffer whose DMA transfer is still in flight.
    sTransDone = xSemaphoreCreateCounting(2, 2);
```

The semaphore's count **is** the number of buffers not currently owned by the DMA engine.
Three rules:

1. **Take before write.** `displayFlush` calls `xSemaphoreTake` *before* `expandChunk`, never
   after. A take that would exceed the wire's pace simply blocks the game task until the
   oldest transfer completes. Back-pressure is automatic and exact.
2. **Give from the ISR.** The completion callback runs in interrupt context and returns one
   count per finished transfer:

   ```c
   static bool IRAM_ATTR onTransDone(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_io_event_data_t* ev, void* user)
   {
       BaseType_t woken = pdFALSE;
       xSemaphoreGiveFromISR(sTransDone, &woken);
       return woken == pdTRUE;
   }
   ```

   It is `IRAM_ATTR` so a cache miss on flash (e.g. during a flash write, or just cold
   I-cache) can never delay or deadlock the completion path.
3. **Drain = take both, give both.** `displayWaitFlush` exploits the counting semantics
   directly — if both counts can be taken, no transfer is in flight:

   ```c
   void displayWaitFlush(void)
   {
       // drain: both buffers free == no transfer in flight
       for (int i = 0; i < 2; i++) xSemaphoreTake(sTransDone, portMAX_DELAY);
       for (int i = 0; i < 2; i++) xSemaphoreGive(sTransDone);
   }
   ```

Because `displayFlush` takes only as it needs buffers, it returns after *queueing* the last
band, with up to two bands still on the wire. That is deliberate: the trailing ~6 ms of wire
time overlaps the next frame's `gameUpdate`/`gameRender` for free (this overlap is why the
title screen slightly beats the naive 1/16.37 ms ceiling — see 08-performance.md).

## 6. What esp_lcd contributes (and the facts the design leans on)

Three properties of `esp_lcd`'s SPI panel IO are ground truth for this design; if any changed,
the protocol above would need re-verification:

- **`draw_bitmap` sends by reference.** The pointer you pass is handed to the DMA engine;
  no staging copy is made. This is what makes the pipeline zero-copy after expansion — and
  exactly why the semaphore protocol must exist: the buffer belongs to the driver until the
  completion callback.
- **Automatic drain before commands.** Each `draw_bitmap` begins with ST7789 window commands
  (`CASET`/`RASET`/`RAMWR`), and the panel IO waits for previously queued color data to finish
  before it will clock out a new command. So the driver has built-in back-pressure of its own
  (`trans_queue_depth = 8` in our IO config is therefore generous, never the limiter). Our
  semaphore is not protecting against driver-side reordering — it protects *our buffer reuse*,
  which the driver knows nothing about.
- **One callback per `draw_bitmap`.** `on_color_trans_done` fires once per draw call, even
  when the driver internally splits a buffer into several SPI transactions (as it would with
  the vendor's `max_transfer_sz=4000`). The give/take bookkeeping counts *draw calls*, not
  transactions — which is exactly right, because buffer ownership is per draw call. Sizing
  bands to one transaction each is a performance decision, not a correctness requirement.

## 7. Backlight sequencing: never show the boot garbage

This panel has no reset GPIO — `PIN_LCD_RST` is `-1`; the board resets the controller with an
RC power-on circuit and we issue a software `SWRESET` via `esp_lcd_panel_reset`. After
power-on, the panel's GRAM contains random data, and the backlight is driven by an SS8050 NPN
transistor on GPIO1, active high — so the board boots **dark** by default. The init sequence
preserves that mercy:

```c
    gpio_set_level(PIN_LCD_BL, !LCD_BL_ON_LEVEL);   // keep it dark
    ...panel init, invert, rotation...

    // push one black frame before enabling the backlight
    memset(sChunk[0], 0, CHUNK_BYTES);
    for (int y = 0; y < DISPLAY_HEIGHT; y += CHUNK_LINES) {
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, DISPLAY_WIDTH, y + CHUNK_LINES, sChunk[0]);
    }
    displayWaitFlush();
    gpio_set_level(PIN_LCD_BL, LCD_BL_ON_LEVEL);
```

Backlight off during init, one full black frame pushed through the *real* pipeline (which
doubles as a smoke test of the whole DMA path before the game starts), a full drain, then
light. The user never sees initialization garbage. Note the black-frame loop is also the
ping-pong protocol in miniature — same take-before-send discipline, reusing `sChunk[0]` only
when the semaphore says the wire is done with it.

Panel configuration itself follows the vendor init verbatim (verified against the schematic
and demo code, see `board_pins.h`): `invert_color(true)` is **mandatory** on this IPS panel
(colors are negative without it), element order RGB not BGR, zero gap offsets, and landscape
comes free via MADCTL — `swap_xy(true)` + `mirror(true, false)` turns the native 240x320
portrait into our 320x240 with zero per-frame cost.

## 8. Sharing the bus with the microSD

The microSD slot sits on the **same SPI2 bus** (MISO=40, CS=41). Three rules keep coexistence
safe, all visible in the init code:

1. **Park SD_CS high from the first instruction of display init** — before the bus even
   exists — so the card can never drive MISO or interpret LCD traffic as a command:

   ```c
   gpio_config_t bl = {
       .pin_bit_mask = (1ULL << PIN_LCD_BL) | (1ULL << PIN_SD_CS),
       .mode = GPIO_MODE_OUTPUT,
   };
   gpio_config(&bl);
   gpio_set_level(PIN_LCD_BL, !LCD_BL_ON_LEVEL);
   // shared SPI2 bus: keep the microSD deselected so it never drives MISO
   gpio_set_level(PIN_SD_CS, 1);
   ```

2. **MISO belongs to the bus config, never to the LCD panel IO.** The LCD link is
   write-only. This is not cosmetic: on the S3, SPI signals routed through the GPIO matrix
   cap at 40 MHz on the *read* path only — writes are fine at 80 MHz. Keeping the LCD IO
   MISO-free is what makes the 80 MHz clock legitimate (and it is what every vendor demo for
   this board ships). A future SD driver attaches as a second device on the same bus at its
   own, lower clock.
3. **Future SD access must respect the pipeline.** Any SD transaction (Phase 6: loading
   levels from card) must be bracketed with `displayWaitFlush()` — the bus arbiter serializes
   devices per transaction, but our band cadence assumes the wire is ours; interleaving SD
   reads mid-frame would show up as flush-time jitter. Loading during pauses/menus is the
   plan of record.

## Key takeaways

- The display API is four functions; the backend behind it is swappable by build config.
  LovyanGFX was evaluated by reading its source and rejected: its paletted path expands on
  the CPU through a generic copy routine anyway, so a framework buys nothing here.
- Indexed color makes per-pixel transforms free: BGR555→RGB565 conversion *and* the SPI byte
  swap are baked into 256 LUT entries at palette-set time. `esp_lcd` does not swap color
  bytes; frameworks that pay a per-frame swap pass (as IDF's own LVGL example does) leave
  ~1 ms/frame on the table at this resolution.
- Band size falls out of one hardware fact: 32,768 bytes max per S3 SPI transaction. 48 lines
  = 30,720 B is the largest 240-divisor under the cap → 5 transactions/frame. The vendor
  demos' `max_transfer_sz=4000` silently makes that 39 — an ~8x overhead trap.
- The counting semaphore *is* the free-buffer count: take before writing a buffer, ISR gives
  it back. This single invariant prevents the expand-outruns-wire corruption race, provides
  exact back-pressure, and makes `displayWaitFlush` a two-take drain.
- `esp_lcd` sends by reference (zero copy), self-drains before each command, and fires exactly
  one callback per `draw_bitmap` — the three facts the protocol's correctness rests on.
- Backlight goes on only after a full black frame has traversed the real pipeline; SD_CS is
  parked high before the bus exists; MISO stays out of the LCD IO so the write-only link can
  run at 80 MHz.
- Measured result: 16.37 ms full-frame flush against a 15.36 ms wire floor — 6% overhead,
  i.e. the pipeline runs at the bus floor. What that buys in game terms is the subject of
  08-performance.md.
