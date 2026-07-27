# OpenLara ESP32-S3 — game firmware component.
#
# TU layout mirrors every working potato build (gba/dos/win sims):
#   game_main.cpp  platform shell + ALL game logic via game.h (header-only engine)
#   render.cpp     copy of gba/render.iwram.cpp — copied HERE so its quoted
#                  #include "rasterizer.h" resolves to our MODE13 forwarder
#   sound.cpp      the GBA mixer (AD4 music + PCM SFX), output wired in Phase 4
#   common.cpp     engine globals
#   display_esplcd.c  ST7789 DMA pipeline (Phase 2, measured 16.37ms/frame)
#
# main.c (the Phase 2 display benchmark) is intentionally NOT compiled — it
# has its own app_main. Re-enable by swapping it for game_main.cpp + engine TUs.

set(ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)   # -> src/
set(RENDER_COPY ${CMAKE_CURRENT_LIST_DIR}/render.cpp)

# configure-time copy (same pattern as dos/deploy.bat and tns/Makefile)
configure_file(
    ${ENGINE_DIR}/platform/gba/render.iwram.cpp
    ${RENDER_COPY}
    COPYONLY
)

idf_component_register(
    SRCS
        "game_main.cpp"
        "render.cpp"
        "${ENGINE_DIR}/platform/gba/sound.cpp"
        "${ENGINE_DIR}/fixed/common.cpp"
        "display_esplcd.c"
    INCLUDE_DIRS "." "${ENGINE_DIR}/fixed"
    REQUIRES
        esp_lcd
        esp_timer
        esp_psram
        esp_partition
        driver
)

# engine sources predate modern warnings; keep the log readable
set_source_files_properties(
    render.cpp
    ${ENGINE_DIR}/platform/gba/sound.cpp
    ${ENGINE_DIR}/fixed/common.cpp
    game_main.cpp
    PROPERTIES COMPILE_OPTIONS "-w;-D__ESP32__"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE __ESP32__)
