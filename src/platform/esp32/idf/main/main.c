// Phase 2 test: display bring-up + flush benchmark.
// Renders a moving 8bpp test pattern with an animated BGR555 palette and
// reports ms per displayFlush — that number is the FPS ceiling of the port.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_partition.h"

#include "display.h"

static const char* TAG = "openlara";

// 8bpp framebuffer in internal SRAM — exactly like the real game will have.
static uint8_t fb8[DISPLAY_WIDTH * DISPLAY_HEIGHT];

// BGR555, like OpenLara's palSet output: bits 0-4 R, 5-9 G, 10-14 B
static uint16_t palette[256];

static void buildPalette(int shift)
{
    for (int i = 0; i < 256; i++) {
        int v = (i + shift) & 0xFF;
        uint16_t r = (v >> 3);                 // ramp
        uint16_t g = ((255 - v) >> 3);         // inverse ramp
        uint16_t b = ((v ^ 0x80) >> 3);        // offset ramp
        palette[i] = r | (g << 5) | (b << 10);
    }
}

static void buildPattern(int frame)
{
    // vertical gradient bands + a moving diagonal — cheap but touches every pixel
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        uint8_t* row = fb8 + y * DISPLAY_WIDTH;
        uint8_t base = (uint8_t)(y + frame);
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            row[x] = (uint8_t)(base + ((x + frame) >> 1));
        }
    }
}

static void checkLevelsPartition(void)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "levels");
    if (!part) {
        ESP_LOGW(TAG, "levels partition not found");
        return;
    }
    const void* map;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h) == ESP_OK) {
        const uint32_t* p = (const uint32_t*)map;
        ESP_LOGI(TAG, "levels partition mapped at %p, size %lu KB, first words: %08lx %08lx",
                 map, (unsigned long)(part->size >> 10),
                 (unsigned long)p[0], (unsigned long)p[1]);
    } else {
        ESP_LOGW(TAG, "levels partition mmap failed");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenLara ESP32-S3 — phase 2 display benchmark");
    ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(esp_psram_get_size() >> 10));
    ESP_LOGI(TAG, "free internal: %u KB, largest block: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10));

    checkLevelsPartition();

    if (displayInit() != 0) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    int frame = 0;
    int64_t accumFlush = 0, accumFrame = 0;
    int64_t lastReport = esp_timer_get_time();

    while (1) {
        int64_t t0 = esp_timer_get_time();

        buildPalette(frame);        // animated palette (fade machinery uses this path)
        displaySetPalette(palette);
        buildPattern(frame);

        int64_t t1 = esp_timer_get_time();
        displayFlush(fb8);
        displayWaitFlush();         // benchmark: measure the full transfer
        int64_t t2 = esp_timer_get_time();

        accumFlush += t2 - t1;
        accumFrame += t2 - t0;
        frame++;

        if (t2 - lastReport >= 1000000) {
            int n = frame ? frame : 1;
            static int lastFrame = 0;
            int frames = frame - lastFrame;
            if (frames > 0) {
                ESP_LOGI(TAG, "fps=%d  flush=%.2f ms  total=%.2f ms",
                         frames,
                         (double)accumFlush / frames / 1000.0,
                         (double)accumFrame / frames / 1000.0);
            }
            (void)n;
            lastFrame = frame;
            accumFlush = accumFrame = 0;
            lastReport = t2;
        }
    }
}
