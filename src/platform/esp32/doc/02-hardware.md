# The Board: Waveshare ESP32-S3-Touch-LCD-2, Fully Mapped

This document is the complete hardware reference for the OpenLara port: every pin the
board uses, every pin it leaves free, and the reasoning behind each display and input
decision. After reading it you should be able to wire the game pad, understand why the
display driver is configured the way it is, and know which GPIOs you may touch without
breaking the board's own peripherals.

Ground truth, in order of authority:

1. The vendor schematic (`ESP32-S3-Touch-LCD-2-SchDoc.pdf`, waveshare.com wiki) — every
   net in this document was read from it.
2. The vendor ESP-IDF demos (`ESP32-S3-Touch-LCD-2-Demo.zip`, the
   `esp_lcd_new_panel_st7789` path) — the source of the panel init sequence and the
   80 MHz SPI clock.
3. `src/platform/esp32/idf/main/board_pins.h` — the authoritative pin map in the repo.
   If this document and that header ever disagree, the header wins.

## 1. What is on the board

| Component | Part | Bus / pins | Port relevance |
|---|---|---|---|
| SoC | ESP32-S3R8: dual Xtensa LX7 @ 240 MHz | — | Two cores; 512 KB internal SRAM |
| PSRAM | 8 MB octal DDR, in-package (AP 64Mbit gen3) | internal, verified 80 MHz | Level data lives here (see 05-memory-map.md) |
| Flash | 16 MB quad W25Q128JVS | dedicated SPI pads, QIO 80 MHz | App + 12.9 MB `levels` partition |
| Display | 2" IPS 240x320, ST7789T3 | SPI2 | The frame ceiling of the whole port |
| Touch | CST816D | I2C0 (SDA=48, SCL=47), addr 0x15, INT=46 | Future menu input |
| IMU | QMI8658 | same I2C0, addr 0x6B, INT1=GPIO3 | Unused so far |
| Camera | OV5640 socket (24-pin DVP, J1) | GPIOs 2,4,6,7,8,9,10,11,12,13,14,15 + SCCB 16/21 + PWDN 17 | **Must be unplugged — its pins are the game pad** |
| microSD | TF slot | **same SPI2 bus as the LCD** (MISO=40, CS=41) | Future extra levels |
| Battery | MX1.25 header, charger + boost | ADC on GPIO5 (VBAT/3 divider) | Portable play |
| USB-C | native USB-Serial-JTAG | GPIO19 (D-), GPIO20 (D+) | Flashing + serial input |
| Buttons | BOOT (GPIO0), RESET | — | BOOT doubles as emergency START+A |

Note what is *not* on the board: no speaker, no DAC, no game buttons. Audio and input
have to come in through the expansion headers.

## 2. The LCD subsystem

### 2.1 Pins

From `board_pins.h` (verified against the schematic's J3 LCD connector):

```c
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SPI_HZ      (80 * 1000 * 1000)

#define PIN_LCD_SCLK    39
#define PIN_LCD_MOSI    38
#define PIN_LCD_CS      45
#define PIN_LCD_DC      42
#define PIN_LCD_RST     (-1)  // no GPIO: RC power-on reset circuit; SWRESET only
#define PIN_LCD_BL      1     // SS8050 NPN driver
#define LCD_BL_ON_LEVEL 1
```

### 2.2 No reset GPIO — the RC circuit

The panel's RESET line (J3 pin 8) is **not connected to any GPIO** in the default board
population. On the schematic it goes to a 10K pull-up (R14) to 3V3 plus a capacitor to
ground (C30, marked "100nF/NC"), forming a power-on reset. An unpopulated resistor
(R16, "NC/0R") *could* tie it to GPIO0/BOOT, but it is not fitted. The touch
controller's TP_RESET (J3 pin 16) hangs off the same net.

Consequence for the driver: `PIN_LCD_RST = -1`, and the panel can only be reset in
software via the ST7789 `SWRESET` command — which is exactly what
`esp_lcd_panel_reset()` does when `reset_gpio_num` is -1. If the panel ever wedges into
a bad state, only a power cycle fully clears it.

### 2.3 Panel quirks (all mandatory, all vendor-verified)

```c
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_INVERT_COLOR true
#define LCD_MIRROR_X     true
#define LCD_MIRROR_Y     false
#define LCD_GAP_X        0
#define LCD_GAP_Y        0
```

- **Inversion ON**: this is an IPS panel; without `INVCMD` the image renders in negative.
  Non-negotiable, and easy to misdiagnose as a palette bug.
- **RGB element order** (not the BGR many ST7789 boards use).
- **Zero gaps**: the panel is a true 240x320, no offset window like the 135x240 modules.
- **Rotation is free**: the panel is natively 240x320 portrait; landscape 320x240 is the
  controller's MADCTL register (vendor "rotation 1" = MX|MV), done once at init:

```c
// display_esplcd.c — native 240x320 portrait -> 320x240 landscape
ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(sPanel, true));
ESP_ERROR_CHECK(esp_lcd_panel_mirror(sPanel, LCD_MIRROR_X, LCD_MIRROR_Y));
```

  The matching touch transform for this rotation is `x' = raw_y`, `y' = 239 - raw_x`.
- **TE not routed**: the panel FPC exposes a tearing-effect pin (J3 pin 10) but the board
  does not route it to any GPIO. No vsync is possible; the display pipeline compensates
  with queued DMA writes instead of racing the scan-out.
- **Backlight boots dark**: GPIO1 drives an SS8050 NPN low-side switch (active high
  through a 1K base resistor). The panel shows garbage RAM at power-on, so
  `displayInit()` pushes one black frame *before* raising the backlight.

### 2.4 Why 80 MHz SPI is safe here (and why the GPIO matrix does not cap it)

Two objections come up against `LCD_SPI_HZ = 80 MHz`; both dissolve on inspection:

1. *"The ST7789 datasheet says ~15-20 MHz."* The vendor's own ESP-IDF demos — every one
   of them — configure this exact panel at 80 MHz. That is a shipped, mass-produced
   configuration for this specific board and routing, not an overclocking experiment.
   Measured on this board: a full 153,600-byte frame flushes in **16.37 ms**, 6% above
   the 15.36 ms wire-speed floor — the link is real and clean at 80 MHz.
2. *"SPI through the GPIO matrix is limited to 40 MHz."* Pins 38/39/42/45 are not SPI2's
   IOMUX pins, so the signal does go through the GPIO matrix — but the documented 40 MHz
   matrix limit protects the **read path**: the mux adds delay that shrinks the MISO
   sampling window. This link is **write-only** (the panel's MISO is never attached to
   the LCD IO, see below), so there is no sampling window to violate. Output-only at
   80 MHz through the matrix is what the vendor demos prove in practice on the S3.

If artifacts ever appear on another unit, the fallback is 40 MHz — at the cost of the
flush doubling to ~31 ms (~32 FPS ceiling).

One vendor-demo trap worth recording: the Waveshare demo sets `max_transfer_sz = 4000`,
which silently splits every frame into 39 SPI transactions (~8x per-transaction
overhead). Our driver sizes the bus to the DMA band instead (30,720 bytes — one
transaction per band, five per frame; see `display_esplcd.c`).

## 3. The shared SPI2 bus: LCD + microSD

The microSD slot sits on the **same SPI2 bus** as the display: it shares SCLK=39 and
MOSI=38, and adds MISO=40 and its own CS=41. This forces three rules, all implemented
in `displayInit()`:

```c
// display_esplcd.c
gpio_set_level(PIN_SD_CS, 1);      // shared SPI2 bus: keep the microSD deselected
...
spi_bus_config_t bus = {
    .sclk_io_num = PIN_LCD_SCLK,
    .mosi_io_num = PIN_LCD_MOSI,
    .miso_io_num = PIN_SD_MISO,    // for the SD card; LCD IO stays write-only
    ...
};
```

1. **SD_CS (GPIO41) is parked high from boot**, before the bus even initializes.
   A floating CS could let the card interpret display traffic as a command and drive
   MISO — or worse, corrupt the card.
2. **MISO=40 belongs to the bus config only, never to the LCD panel IO.** The LCD link
   stays write-only, which is precisely what legitimizes 80 MHz through the GPIO matrix
   (section 2.4). SD transactions, when they come, are a separate SPI device on the same
   host with their own (slower) clock.
3. **SD access and display DMA cannot overlap.** `esp_lcd` queues transactions on the
   host; an SDSPI device on the same host interleaves with them, so level loads from SD
   must happen at pauses (loading screens), never mid-frame. Today levels live in a flash
   partition, so this is a future-proofing rule, not a current constraint.

## 4. The expansion headers, pin by pin

Two 14-pin headers break out every free GPIO. These lists were read from the schematic
net list (connector P1/P2 against the ESP32-S3R8 symbol); the P1 order matches the
comment in `board_pins.h`.

### Header P1

| P1 pin | Signal | Board function | Port use |
|---|---|---|---|
| 1 | IO2 | CAM_D7 | **BTN_UP** |
| 2 | IO4 | CAM_HREF | **BTN_DOWN** |
| 3 | IO6 | CAM_VSYNC | **BTN_LEFT** |
| 4 | IO16 | camera SCCB SCL (4.7K pull-up R4) | **BTN_RIGHT** |
| 5 | IO17 | CAM_PWDN (10K pull-down R6 — see 5.2) | **BTN_A** |
| 6 | IO18 | — (unencumbered) | **BTN_B** |
| 7 | IO21 | camera SCCB SDA (4.7K pull-up R5) | **BTN_L** |
| 8 | IO8 | CAM_XCLK | **BTN_R** |
| 9 | IO7 | CAM_D6 | **BTN_START** |
| 10 | IO10 | CAM_D5 | **BTN_SELECT** |
| 11 | IO20 | USB D+ (22R series) | do not use |
| 12 | IO19 | USB D- (22R series) | do not use |
| 13 | GND | — | **pad common ground** |
| 14 | 5V | — | — |

### Header P2

| P2 pin | Signal | Board function | Port use |
|---|---|---|---|
| 1 | 3V3 | — | — |
| 2 | GND | — | — |
| 3 | IO43 | UART0 TXD | free (carries console log by default) |
| 4 | IO44 | UART0 RXD | free |
| 5 | IO47 | I2C SCL (touch + IMU, 4.7K pull-up) | do not repurpose |
| 6 | IO48 | I2C SDA (touch + IMU, 4.7K pull-up) | do not repurpose |
| 7 | IO15 | CAM_D2 | free (camera unplugged) |
| 8 | IO13 | CAM_D1 | free (camera unplugged) |
| 9 | IO11 | CAM_D3 | free (camera unplugged) |
| 10 | IO12 | CAM_D0 | free (camera unplugged) |
| 11 | IO14 | CAM_D4 | free (camera unplugged) |
| 12 | IO9 | CAM_PCLK | free (camera unplugged) |
| 13 | GND | — | — |
| 14 | VBAT | battery rail | — |

## 5. The game pad on P1

### 5.1 The assignment

All ten buttons fit on **one header**, consecutive pins 1-10, with the common ground on
pin 13 of the same header — a game pad is one ribbon cable. Buttons are **active low**:
each button shorts its P1 pin to P1 pin 13; the GPIO reads with a pull-up
(`game_main.cpp` enables internal pull-ups on all of them):

```c
// board_pins.h
#define PIN_BTN_UP      2   // P1 pin 1
#define PIN_BTN_DOWN    4   // P1 pin 2
#define PIN_BTN_LEFT    6   // P1 pin 3
#define PIN_BTN_RIGHT   16  // P1 pin 4  (hw pullup)
#define PIN_BTN_A       17  // P1 pin 5
#define PIN_BTN_B       18  // P1 pin 6
#define PIN_BTN_L       21  // P1 pin 7  (hw pullup)
#define PIN_BTN_R       8   // P1 pin 8
#define PIN_BTN_START   7   // P1 pin 9
#define PIN_BTN_SELECT  10  // P1 pin 10
```

GPIO0 (the on-board BOOT key) additionally acts as an emergency START+A, so the game is
navigable with zero soldering.

### 5.2 Why IO16 and IO21 are special

They are the camera's SCCB (I2C-flavored) bus, so the board fits **4.7K hardware
pull-ups** on them. For active-low buttons that is a feature: those two lines idle high
with a stiffer pull than the S3's internal ~45K, giving better noise margin on ribbon
wiring. The firmware still enables internal pull-ups on everything uniformly — harmless
in parallel.

The mirror image of that coin is **IO17 (BTN_A)**: the schematic fits a **10K pull-down
(R6) to GND** on it, because it is the camera's PWDN line and must default to
"powered down". A 10K pull-down against only the internal ~45K pull-up puts the idle
node around 0.6 V — likely below the S3's V_IL, i.e. the pin may read as *constantly
pressed*. **This is flagged as unverified**: the button firmware is built but the
physical pad had not been wired at the time of writing. If BTN_A misbehaves, the fixes
are (a) a strong external pull-up (1-2.2K) on the button line, or (b) reassigning A to a
clean pin from P2 (IO9/IO11/IO12/IO13/IO14/IO15) and updating `board_pins.h`.

### 5.3 Why the camera must be unplugged

Every d-pad and face-button GPIO except IO18 *is* a camera line (see the P1 table:
D5/D6/D7, HREF, VSYNC, XCLK, PWDN, and the SCCB pair). With an OV5640 module seated in
J1, the sensor actively drives its data/sync outputs — pressing a button would short a
driven output to ground through the button, and the sensor would see clocks and resets
it never asked for. Camera and game pad are mutually exclusive by construction; the
port chose the pad.

## 6. Free vs. forbidden GPIOs

| GPIO | Bound to | Verdict |
|---|---|---|
| 0 | BOOT key, boot strap | usable as input (used: emergency START+A) |
| 1 | LCD backlight driver | forbidden |
| 2, 4, 6, 7, 8, 10 | camera DVP / P1 | **game pad** (camera unplugged) |
| 3 | IMU INT1 output, JTAG-sel strap | avoid — the IMU drives it |
| 5 | battery divider (VBAT/3) | reserved for battery ADC |
| 9, 11, 12, 13, 14, 15 | camera DVP / P2 | free (camera unplugged) |
| 16, 21 | camera SCCB / P1, 4.7K pull-ups | **game pad** |
| 17 | CAM_PWDN / P1, 10K pull-down | **game pad** (see 5.2 caveat) |
| 18 | P1 only, unencumbered | **game pad** |
| 19, 20 | USB D-/D+ | forbidden (flashing + serial input path) |
| 26-32 | quad flash | not exposed |
| 33-37 | in-package octal PSRAM | not exposed / unavailable |
| 38, 39 | SPI2 MOSI/SCLK (LCD + SD) | forbidden |
| 40, 41 | SD MISO / SD CS | forbidden (41 must stay high) |
| 42, 45 | LCD DC / LCD CS (45 is a VDD_SPI strap) | forbidden |
| 43, 44 | UART0 TX/RX on P2 | free-ish (console log rides TX by default) |
| 46 | touch INT, boot strap | reserved for touch |
| 47, 48 | I2C0 SCL/SDA (touch + IMU) | reserved; only for adding I2C devices |

Bottom line: with the camera unplugged the board offers **16 clean header GPIOs**
(2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21) plus the semi-free 43/44 —
ten of which the game pad consumes, leaving six spare for future I2S audio
(MAX98357A needs three) with room left over.

## Key takeaways

- The pin map lives in `src/platform/esp32/idf/main/board_pins.h`, verified against the
  vendor schematic and demos; this document explains it, the header defines it.
- The LCD has **no reset GPIO** (RC power-on circuit, `SWRESET` only), needs
  **inversion ON**, **RGB order**, and gets landscape for free via MADCTL
  (`swap_xy` + mirror X); touch coordinates need the matching transform.
- **80 MHz SPI is vendor-proven** on this exact board; the GPIO-matrix 40 MHz limit is a
  read-path constraint and this link is write-only. Measured flush: 16.37 ms/frame.
- The microSD **shares SPI2**: park SD_CS high from boot, keep MISO out of the LCD panel
  IO, never overlap SD access with display DMA.
- All ten game buttons live on **header P1 pins 1-10** with ground on pin 13; active
  low with pull-ups. IO16/IO21 have hardware 4.7K pull-ups (SCCB); IO17 has a 10K
  pull-down that may need an external pull-up — verify on first hardware test.
- The **camera must stay unplugged**: the pad's GPIOs are the DVP bus.
- Never touch GPIOs 1, 19/20, 38-42, 45-48; GPIO0/3/45/46 are strapping pins.
- Sibling docs: memory layout in 05-memory-map.md; the research behind the display and
  memory decisions in 09-research-notes.md.
