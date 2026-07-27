// OpenLara ESP32-S3 platform layer.
// Mirrors the __ESP32_WIN__ Windows harness (src/platform/esp32/win/main.cpp)
// with ESP-IDF equivalents:
//   file loads  -> OLVL container in the mmap'd "levels" flash partition,
//                  level blob memcpy'd to PSRAM (writable; engine mutates it)
//   blit        -> displayFlush(fb) through the esp_lcd DMA pipeline
//   settings    -> NVS (stubbed to defaults for now)
//   input       -> BOOT button as START/A placeholder until the GPIO pad lands
#include "game.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "driver/gpio.h"

#include "driver/usb_serial_jtag.h"

#include "display.h"
#include "board_pins.h"

static const char* TAG = "openlara";

int32 fps;
int32 frameIndex = 0;
int32 fpsCounter = 0;
uint32 curSoundBuffer = 0;

const void* TRACKS_AD4;
const void* TITLE_SCR;
static uint8* levelData;      // current level blob in PSRAM

// ---- OLVL container (built by make_levels.py, mmap'd from flash) ----------
#define OLVL_MAGIC 0x4C564C4F

struct OlvlEntry {
    char name[16];
    uint32 offset;
    uint32 size;
};

static const uint8* sLevelsBase;   // mmap'd partition base
static const OlvlEntry* sFiles;
static uint32 sFileCount;

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
    if (hdr[0] != OLVL_MAGIC) {
        ESP_LOGE(TAG, "levels partition: bad magic %08lx", (unsigned long)hdr[0]);
        return false;
    }

    sLevelsBase = (const uint8*)map;
    sFileCount  = hdr[1];
    sFiles      = (const OlvlEntry*)(sLevelsBase + 8);
    ESP_LOGI(TAG, "levels: %lu files mounted", (unsigned long)sFileCount);
    return true;
}

static const OlvlEntry* levelsFind(const char* name)
{
    for (uint32 i = 0; i < sFileCount; i++) {
        if (strcmp(sFiles[i].name, name) == 0)
            return &sFiles[i];
    }
    return NULL;
}

// ---- os* contract ---------------------------------------------------------
void osSetPalette(const uint16* palette)
{
    displaySetPalette(palette);
}

int32 osGetSystemTimeMS()
{
    return (int32)(esp_timer_get_time() / 1000);
}

// settings/saves: NVS later (Phase 6); defaults are fine meanwhile
bool osSaveSettings() { return false; }
bool osLoadSettings() { return false; }
bool osCheckSave()    { return false; }
bool osSaveGame()     { return false; }
bool osLoadGame()     { return false; }
void osJoyVibrate(int32 index, int32 L, int32 R) {}

const void* osLoadScreen(LevelID id)
{
    return TITLE_SCR;
}

const void* osLoadLevel(LevelID id)
{
    char buf[32];
    sprintf(buf, "%s.PKD", (const char*)gLevelInfo[id].data);

    const OlvlEntry* e = levelsFind(buf);
    if (!e) {
        ESP_LOGE(TAG, "level not found: %s", buf);
        return NULL;
    }

    // the engine mutates the level image (tile fixups, animated textures) and
    // needs a pristine copy per (re)load — free the old one, copy from flash.
    // PSRAM serves random texture reads 3-4x faster than flash and keeps the
    // flash side of the shared SPI0 bus free for code fetch.
    free(levelData);
    levelData = (uint8*)heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM);
    if (!levelData) {
        ESP_LOGE(TAG, "no PSRAM for level (%lu bytes)", (unsigned long)e->size);
        return NULL;
    }

    int64_t t0 = esp_timer_get_time();
    memcpy(levelData, sLevelsBase + e->offset, e->size);
    ESP_LOGI(TAG, "%s: %lu KB -> PSRAM in %lld ms", buf,
             (unsigned long)(e->size >> 10), (esp_timer_get_time() - t0) / 1000);

    return levelData;
}

// ---- input: serial keys + BOOT button (full GPIO pad in Phase 4) ----------
// Serial control over USB (for development without soldered buttons):
//   w/s/a/d = d-pad   x = A (action)   z = B (jump)   q = L   e = R (walk)
//   Enter = START (inventory/confirm)   space = SELECT
// Each received char holds its key for a few frames (serial has no key-up).

#define SERIAL_HOLD_FRAMES 8

static uint8 sHold[16]; // per-IK_* bit countdown

// {gpio, IK_* bit} — physical pad on header P1 (see board_pins.h)
static const struct { uint8 gpio; uint32 mask; } sButtons[] = {
    { PIN_BTN_UP,     IK_UP     },
    { PIN_BTN_DOWN,   IK_DOWN   },
    { PIN_BTN_LEFT,   IK_LEFT   },
    { PIN_BTN_RIGHT,  IK_RIGHT  },
    { PIN_BTN_A,      IK_A      },
    { PIN_BTN_B,      IK_B      },
    { PIN_BTN_L,      IK_L      },
    { PIN_BTN_R,      IK_R      },
    { PIN_BTN_START,  IK_START  },
    { PIN_BTN_SELECT, IK_SELECT },
};

static void inputInit()
{
    uint64_t mask = 1ULL << GPIO_NUM_0; // BOOT
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        mask |= 1ULL << sButtons[i].gpio;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE; // 16/21 also have 4.7K on board
    gpio_config(&cfg);

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&usb_cfg);
}

static int keyBit(char c)
{
    switch (c) {
        case 'w': return 0;   // IK_UP     = 1<<0
        case 'd': return 1;   // IK_RIGHT
        case 's': return 2;   // IK_DOWN
        case 'a': return 3;   // IK_LEFT
        case 'x': return 4;   // IK_A
        case 'z': return 5;   // IK_B
        case 'q': return 10;  // IK_L
        case 'e': return 11;  // IK_R
        case '\r':
        case '\n': return 14; // IK_START
        case ' ': return 15;  // IK_SELECT
    }
    return -1;
}

static void inputUpdate()
{
    uint8 buf[16];
    int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
    for (int i = 0; i < n; i++) {
        int bit = keyBit((char)buf[i]);
        ESP_LOGI(TAG, "rx '%c' -> bit %d (inv state %d)",
                 buf[i] >= 32 ? buf[i] : '?', bit, (int)inventory.state);
        if (bit >= 0) sHold[bit] = SERIAL_HOLD_FRAMES;
    }

    uint32 k = 0;
    for (int i = 0; i < 16; i++) {
        if (sHold[i]) { sHold[i]--; k |= 1u << i; }
    }

    // physical pad (active low)
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        if (!gpio_get_level((gpio_num_t)sButtons[i].gpio)) k |= sButtons[i].mask;
    }

    // BOOT button (active low) = START+A, still works standalone
    if (!gpio_get_level(GPIO_NUM_0)) k |= IK_START | IK_A;

    keys = k;
}

// ---- game task ------------------------------------------------------------
static void gameTask(void*)
{
    if (!levelsMount()) {
        ESP_LOGE(TAG, "levels partition missing/corrupt — flash it with flash.ps1 -Levels");
        vTaskDelete(NULL);
    }

    // TRACKS.AD4 stays flash-mapped: the mixer reads it sequentially
    const OlvlEntry* tracks = levelsFind("TRACKS.AD4");
    TRACKS_AD4 = tracks ? sLevelsBase + tracks->offset : NULL;

    // no 320x240 TITLE.SCR asset yet -> black background (PSRAM, zeroed)
    TITLE_SCR = heap_caps_calloc(1, FRAME_WIDTH * FRAME_HEIGHT, MALLOC_CAP_SPIRAM);

    if (displayInit() != 0) {
        ESP_LOGE(TAG, "display init failed");
        vTaskDelete(NULL);
    }

    inputInit();

    ESP_LOGI(TAG, "free internal after init: %u KB (largest %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10));

    gameInit();

    ESP_LOGI(TAG, "game up — free internal: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));

    int64_t startTime = esp_timer_get_time() / 1000 - 33;
    int32 lastFrame = 0;
    int64_t fpsTime = esp_timer_get_time();

    while (1) {
        inputUpdate();

        int32 frame = (int32)((esp_timer_get_time() / 1000 - startTime) / 33);
        gameUpdate(frame - lastFrame);
        lastFrame = frame;

        gameRender();

        displayFlush((const uint8_t*)fb);

        fpsCounter++;
        int64_t now = esp_timer_get_time();
        if (now - fpsTime >= 1000000) {
            fps = fpsCounter;
            fpsCounter = 0;
            fpsTime += 1000000;
            ESP_LOGI(TAG, "fps=%ld", (long)fps);
        }

        // let IDLE run (watchdog) — one tick at 1000Hz
        vTaskDelay(1);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "OpenLara ESP32-S3");
    // 32KB stack, internal RAM, pinned to core 0 (core 1 reserved for
    // audio mixing + future display offload)
    xTaskCreatePinnedToCore(gameTask, "game", 32 * 1024, NULL, 5, NULL, 0);
}
