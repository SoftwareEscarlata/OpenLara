// Waveshare ESP32-S3-Touch-LCD-2 pin map — VERIFIED against the vendor
// schematic (ESP32-S3-Touch-LCD-2-SchDoc.pdf) and the ESP-IDF demos in
// ESP32-S3-Touch-LCD-2-Demo.zip (esp_lcd_new_panel_st7789 path).
#pragma once

#include "driver/spi_master.h"
#include "hal/lcd_types.h"

// ---- LCD: ST7789T3, 240x320 native portrait, SPI2 (shared with microSD) ----
#define LCD_SPI_HOST    SPI2_HOST
// 80 MHz is what ALL vendor ESP-IDF demos ship (write-only link; reads would
// cap at 40). Rebuild with 40 MHz only to compare benchmark numbers.
#define LCD_SPI_HZ      (80 * 1000 * 1000)

#define PIN_LCD_SCLK    39
#define PIN_LCD_MOSI    38
#define PIN_LCD_CS      45
#define PIN_LCD_DC      42
#define PIN_LCD_RST     (-1)  // no GPIO: RC power-on reset circuit; SWRESET only
#define PIN_LCD_BL      1     // SS8050 NPN driver
#define LCD_BL_ON_LEVEL 1

// vendor init: RGB element order, inversion ON (IPS), no gaps.
// Landscape 320x240 = vendor "rotation 1": MADCTL MX|MV -> swap_xy + mirror X.
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_INVERT_COLOR true
#define LCD_MIRROR_X     true
#define LCD_MIRROR_Y     false
#define LCD_GAP_X        0
#define LCD_GAP_Y        0

// ---- microSD: SDSPI on the SAME SPI2 bus ----
// MISO=40 belongs to the bus config (SD only — never to the LCD panel IO).
// SD_CS must be held high from boot so the card stays off the bus.
#define PIN_SD_MISO     40
#define PIN_SD_CS       41

// ---- Touch: CST816D, I2C0 (shared with QMI8658 IMU @0x6B) ----
// Coord transform for landscape rotation 1: x' = raw_y, y' = 239 - raw_x
#define PIN_TOUCH_SDA   48
#define PIN_TOUCH_SCL   47
#define PIN_TOUCH_INT   46    // wired; vendor demos poll instead (chip NAKs while asleep)
#define TOUCH_I2C_ADDR  0x15

// ---- Free GPIOs for game buttons (camera unplugged) ----
// 2,4,6,7,8,9,10,11,12,13,14,15,16,17,18,21  (16/21 already have 4.7K pullups)
// plus GPIO0 (BOOT key, active low). Avoid: 19/20 (USB), 43/44 (UART0),
// 46/47/48 (touch/I2C), 5 (battery ADC), 3 (IMU INT), 35-37 (octal PSRAM).
