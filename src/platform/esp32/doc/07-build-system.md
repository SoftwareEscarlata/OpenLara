# Building the Firmware: ESP-IDF Without idf.py

This document explains how the ESP32-S3 firmware in `src/platform/esp32/idf/` is
actually built, flashed and monitored: why the project drives `cmake` + `ninja`
directly instead of using `idf.py`, the exact environment recipe that makes ESP-IDF
v5.5.4 reliable on this Windows machine, a walkthrough of `build.ps1`, every flag in
`sdkconfig.defaults` and why it is there (with special attention to the cache flags,
which are not IDF defaults), the flash partition layout, the `OLVL` level-container
format produced by `make_levels.py`, the `flash.ps1` offsets, and how to watch (and
play!) the game over serial.

The engine-side counterpart of this document is `03-windows-simulator.md`; the memory
consequences of these choices are covered in `05-memory-map.md`.

## 1. Why cmake + ninja directly, and not idf.py

`idf.py` is a convenience wrapper: it re-derives the environment, activates a Python
virtualenv, then invokes exactly the same `cmake -G Ninja` and `ninja` that anyone can
invoke by hand. On a clean Linux box the wrapper is fine. On a real Windows development
machine — multiple Espressif installs of different vintages, several Pythons, MSYS2 on
`PATH`, PowerShell as the shell — each layer of re-derivation is one more place for the
build to pick the wrong interpreter, the wrong toolchain, or refuse to run at all.

The approach used here inverts that: **the environment is set explicitly, once, in a
7-line block of `build.ps1`, and then plain `cmake` and `ninja` are invoked.** There is
no magic; the root `CMakeLists.txt` is the standard IDF entry point:

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openlara_esp32s3)
```

`project.cmake` is the same file `idf.py` would load. Everything IDF does —
Kconfig processing, component discovery, linker script generation — still happens.
What is removed is only the wrapper's environment guessing.

This exact recipe (paths included) was not invented for OpenLara: it is the known-good
environment used by velxio's `espidf_compiler` on this same machine, transplanted
verbatim. Reusing a proven environment is itself a reliability decision — the first
firmware build succeeded on the first try.

## 2. The environment recipe

From `src/platform/esp32/idf/build.ps1`:

```powershell
$IDF_PATH  = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4"
$TOOLS     = "C:\Espressif5.5"
$PY_VENV   = "C:\Espressif\python_env\idf4.4_py3.10_env"

$env:IDF_PATH            = $IDF_PATH
$env:IDF_TOOLS_PATH      = $TOOLS
$env:IDF_TARGET          = "esp32s3"
$env:IDF_PYTHON_ENV_PATH = $PY_VENV
$env:VIRTUAL_ENV         = $PY_VENV
# IDF 5.x idf_tools.py fatals if MSYSTEM is present
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
```

| Variable | Value | Why |
|---|---|---|
| `IDF_PATH` | `C:\Espressif5.5\frameworks\esp-idf-v5.5.4` | The framework itself; `CMakeLists.txt` includes `$ENV{IDF_PATH}/tools/cmake/project.cmake` |
| `IDF_TOOLS_PATH` | `C:\Espressif5.5` | Root of the tools tree (xtensa gcc, cmake, ninja, ccache live under `$TOOLS\tools\...`) |
| `IDF_TARGET` | `esp32s3` | Chip selection; also passed as `-DIDF_TARGET` to cmake so it cannot drift |
| `IDF_PYTHON_ENV_PATH` | `C:\Espressif\python_env\idf4.4_py3.10_env` | The Python IDF's build scripts run with |
| `VIRTUAL_ENV` | same | Some IDF tooling checks this instead; setting both closes the gap |
| `MSYSTEM` | **removed** | See below |

Two quirks deserve their own paragraphs.

**The "4.4 venv for a 5.x IDF" quirk.** The virtualenv directory is named
`idf4.4_py3.10_env` — it was created by an IDF 4.4-era installer — but it has the
**v5.x Python requirements installed into it**. IDF's build scripts check which
packages the interpreter can import, not what the folder is called, so this works
perfectly. It is kept (rather than creating a fresh `idf5.5` venv) because it is the
interpreter already proven by hundreds of velxio builds on this machine, and it also
carries `esptool` and `pyserial`, which `flash.ps1` and serial play reuse. If you
reproduce this setup elsewhere, any venv satisfying
`$IDF_PATH/tools/requirements/requirements.core.txt` will do — the name is irrelevant.

**The MSYSTEM scrub.** If `MSYSTEM` is set in the environment (it leaks out of any
Git-Bash/MSYS2 shell, and on this machine MSYS2 is a daily driver), IDF 5.x's
`idf_tools.py` detects an "MSYS environment", which is unsupported, and exits fatally —
even though the actual shell is PowerShell. Deleting the variable before configuring is
the entire fix. This is the single most mysterious failure mode in the whole toolchain:
the error implies your shell is wrong when only an inherited variable is.

Then the `PATH` is prepended, in this order:

```powershell
$prepend = @(
    "$PY_VENV\Scripts",                                          # 1. IDF python + esptool
    (Get-ChildItem "$TOOLS\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin" ...),  # 2. cross-gcc
    (Get-ChildItem "$TOOLS\tools\cmake\*\bin" ...),              # 3. cmake
    (Get-ChildItem "$TOOLS\tools\ninja\*" ...),                  # 4. ninja
    (Get-ChildItem "$TOOLS\tools\ccache\*\*" ...)                # 5. ccache
)
$env:PATH = ($prepend -join ";") + ";" + $env:PATH
```

Order matters: the venv's `Scripts` goes first so that the bare name `python` resolves
to the IDF interpreter, ahead of any system Python. The `Get-ChildItem` globs make the
script robust to tool version bumps (the versioned directory under each tool changes;
the glob picks whatever is installed).

## 3. build.ps1 walkthrough

```
powershell -ExecutionPolicy Bypass -File build.ps1 [clean]
```

After the environment block, the script is deliberately boring:

1. `clean` argument: removes the `build/` directory only. (Note what it does *not*
   remove — see the `sdkconfig` warning in section 5.)
2. Configure:

   ```powershell
   cmake -G Ninja -Wno-dev `
       "-DIDF_TARGET=esp32s3" `
       "-DCMAKE_BUILD_TYPE=Release" `
       "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults" `
       "-DCCACHE_ENABLE=1" `
       -S $proj -B $build
   ```

   `-DSDKCONFIG_DEFAULTS` points IDF at our defaults file explicitly;
   `-DCCACHE_ENABLE=1` turns on compiler caching (the ccache binary is already on
   `PATH` from step 5 above) — warm rebuilds of the IDF component tree drop from
   minutes to seconds.
3. `ninja -C $build` — the actual build. Both steps check `$LASTEXITCODE` and `throw`,
   so a failed configure can never masquerade as a successful build.

Output: `build\openlara_esp32s3.bin` plus `build\bootloader\bootloader.bin` and
`build\partition_table\partition-table.bin` — the three images `flash.ps1` writes.

### The component CMake and the render-copy trick

`main/CMakeLists.txt` registers the five translation units that every working potato
build of this engine uses (the same layout as the Windows simulator's Makefile):

| TU | Source | Role |
|---|---|---|
| `game_main.cpp` | local | platform shell; includes `game.h`, i.e. the whole header-only engine |
| `render.cpp` | **copied** from `src/platform/gba/render.iwram.cpp` | the generic software renderer |
| `sound.cpp` | `src/platform/gba/sound.cpp` | AD4 music + PCM SFX mixer (pure-C path) |
| `common.cpp` | `src/fixed/common.cpp` | engine globals |
| `display_esplcd.c` | local | ST7789 DMA pipeline (measured 16.37 ms/frame) |

The copy is done at configure time:

```cmake
configure_file(
    ${ENGINE_DIR}/platform/gba/render.iwram.cpp
    ${RENDER_COPY}
    COPYONLY
)
```

Why copy instead of compiling the file in place? `render.iwram.cpp` line 61 does
`#include "rasterizer.h"` — a *quoted* include, which resolves relative to the
directory of the including file. Compiled in place it would pick up the **GBA**
rasterizer; copied next to `main/rasterizer.h` (a forwarder to the shared
`src/platform/esp32/rasterizer.h`) it picks up ours. The DOS port (`deploy.bat`), the
TI-Nspire port (its Makefile) and our Windows sim all perform the same copy — it is the
engine's established multi-platform mechanism, not a hack invented here.

The component `REQUIRES esp_lcd esp_timer esp_psram esp_partition driver`, and the
engine TUs are compiled with `-w -D__ESP32__` (the sources predate modern warnings;
`-w` keeps the log readable). Note that `main/main.c` — the Phase 2 display benchmark,
which has its own `app_main` — is intentionally **not** in `SRCS`.

## 4. sdkconfig.defaults — every flag, and why

```
CONFIG_IDF_TARGET="esp32s3"
```
Pins the chip so a stray `IDF_TARGET` in the environment can never retarget the build.

**CPU**

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
```
Both LX7 cores at the maximum 240 MHz. A software rasterizer is CPU-bound; there is no
reason to leave the default (lower) boot frequency.

**Flash**

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
```
The board's W25Q128 is 16 MB quad flash; QIO at 80 MHz is its fastest supported mode.
Flash speed matters doubly here: code executes from flash through the cache, and the
level data is read from a flash partition at every level load. (Don't be alarmed that
the *generated* sdkconfig shows `CONFIG_ESPTOOLPY_FLASHMODE="dio"` alongside
`FLASHMODE_QIO=y` — the ROM loader always starts in DIO and the QIO switch happens
later in boot; this is normal IDF behavior.)

**PSRAM**

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```
The S3R8 has 8 MB of octal PSRAM in-package; `MODE_OCT` + `SPEED_80M` runs it in DDR at
80 MHz (~84 MB/s sequential, per published esp32.com measurements). Two of these flags
encode policy, not capability:

- `SPIRAM_USE_CAPS_ALLOC`: PSRAM is available via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
  **only** — IDF must not quietly place task stacks or general `malloc` traffic there.
  Level blobs (up to 2.62 MB) are placed in PSRAM deliberately; nothing lands there by
  accident.
- `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`: enables `EXT_RAM_BSS_ATTR`, which
  `common.h` maps to the port's `EWRAM_COLD` macro. The engine's large *cold* arrays
  (`gBackgroundCopy` 75 KB, `gSaveData` 8 KB, `dynSectors` 24.5 KB, `enemiesExtra`
  ~25 KB) are moved to PSRAM this way, freeing ~130 KB of internal SRAM. Before this,
  the engine's `.dram0.bss` overflowed the internal `dram0_0_seg` by 2528 bytes at
  link time. Details in `05-memory-map.md`.

**Caches — the flags that are not defaults**

```
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_8WAYS=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_8WAYS=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

The IDF v5.5.4 Kconfig (`components/esp_system/port/soc/esp32s3/Kconfig.cache`)
defaults the S3 to a **16 KB instruction cache, 32 KB data cache, 32-byte lines**
(8-way associativity *is* the default for both). This project maxes everything out,
trading reserved SRAM for hit rate:

- **D-cache 64 KB**: the rasterizer streams texture reads out of a multi-megabyte
  level image in PSRAM; every cache miss costs ~0.5 µs. Doubling the D-cache is paid
  for with 32 KB of internal SRAM that would otherwise go to the heap — worth it for
  a texture-bound renderer.
- **D-cache line 64 B**: this one is *required-safe*, not a tuning knob. Octal DDR
  PSRAM transfers in 64-byte wrap bursts; running it with a 32-byte cache line is
  documented to corrupt the cache (arduino-esp32 issue #12480). Independently, the
  elect-gombe benchmark measured +49% sequential throughput for 64 B vs 32 B lines.
- **I-cache 32 KB**: the engine is ~100k lines executing from flash; the bigger
  I-cache keeps the hot render/logic loops resident.

Two practical lessons attached to these flags:

1. **The symbol names contain the number.** The associativity symbols are
   `CONFIG_ESP32S3_DATA_CACHE_8WAYS` / `CONFIG_ESP32S3_INSTRUCTION_CACHE_8WAYS` —
   choice symbols, not `..._WAYS=8` value assignments. Guess the name wrong and
   nothing complains: **unknown symbols in `sdkconfig.defaults` are silently
   ignored.** They are pinned explicitly here (even the 8-way defaults) so the
   intended cache geometry is stated in one place, in correct syntax.
2. **Always verify the generated `sdkconfig`.** The defaults file is an input, not
   the truth. After configuring, check the output — this project's generated
   `sdkconfig` shows the flags took effect:

   ```
   CONFIG_ESP32S3_DATA_CACHE_64KB=y
   CONFIG_ESP32S3_DATA_CACHE_SIZE=0x10000
   CONFIG_ESP32S3_DATA_CACHE_8WAYS=y
   CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
   CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64
   ```

**Partition table**

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```
Points IDF at the project's own table (section 5) instead of the stock 2 MB layout,
which has no room for a 3 MB app plus ~10 MB of level data.

**Compiler / RTOS / misc**

```
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```
`-O2` for the whole build. The default (`-Og`) leaves a software renderer badly
underclocked.

```
CONFIG_FREERTOS_HZ=1000
```
1 ms scheduler tick. The game loop's `vTaskDelay(1)` (which lets the IDLE task feed
watchdogs) then costs at most ~1 ms instead of the default 10 ms granularity — at
48-63 FPS, a 10 ms forced sleep per frame would be ruinous.

```
CONFIG_ESP_TASK_WDT_INIT=n
```
The task watchdog is not started. The game task legitimately monopolizes core 0, and
during display profiling the watchdog's periodic warnings distorted measurements. (A
shipping build could re-enable it and subscribe the game task properly; for
development it is noise.)

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```
INFO-level logging: level-load timing, heap reports and the per-second FPS line stay
visible on the serial console without drowning it in debug spam.

Nothing disables WiFi/Bluetooth explicitly — those stacks simply are not linked
because no component requires them, which keeps ~50 KB of internal SRAM free.

**A trap to remember:** `sdkconfig.defaults` is consulted only when the generated
`sdkconfig` does not exist yet. Editing a default later does nothing until you delete
`sdkconfig` (it lives next to `build.ps1`; `build.ps1 clean` does *not* remove it) and
reconfigure. If a flag change seems to have no effect, this is why.

## 5. partitions.csv — the flash map

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x300000,
# raw level data: TITLE.PKD + GYM.PKD + LEVEL1.PKD + LEVEL2.PKD + TRACKS.AD4 (~9.8MB) + headroom
levels,   data, 0x40,    0x310000, 0xCE0000,
```

Laid over the 16 MB flash (the bootloader and partition table are not rows in the CSV
— they live below 0x9000 at fixed offsets; on the S3 the second-stage bootloader
starts at 0x0):

```
0x000000 +--------------------------------------+
         | second-stage bootloader              |
0x008000 +--------------------------------------+
         | partition table                      |
0x009000 +--------------------------------------+
         | nvs        (24 KB)  settings/saves   |  <- reserved for Phase 6
0x00F000 +--------------------------------------+
         | phy_init   (4 KB)   RF calibration   |
0x010000 +--------------------------------------+
         | factory    (3 MB)                    |
         |   openlara_esp32s3.bin               |
0x310000 +--------------------------------------+
         | levels     (12.875 MB, subtype 0x40) |
         |   OLVL container:                    |
         |   TITLE.PKD  GYM.PKD  LEVEL1.PKD     |
         |   LEVEL2.PKD TRACKS.AD4  (~9.76 MB)  |
         |   + ~3 MB headroom for more levels   |
0xFF0000 +--------------------------------------+
         | unused (64 KB)                       |
0x1000000+--------------------------------------+
```

Design notes:

- **3 MB app partition**: generous for the current binary, sized so debug builds and
  future assets-in-app experiments never force a repartition (repartitioning
  invalidates the levels partition offset baked into `flash.ps1`).
- **`levels` is a raw data partition with custom subtype `0x40`** — no filesystem.
  The firmware finds it with
  `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "levels")` and maps the
  whole thing with `esp_partition_mmap` (`game_main.cpp:48-70`). A filesystem would
  add code, RAM and mount time to deliver the one feature we need — named blobs —
  which the 24-byte-per-file OLVL table already provides, mmap-consumable in place.
- The current content is ~9.76 MB, leaving ~3.1 MB of headroom for additional levels
  before the layout has to change.

## 6. make_levels.py and the OLVL container format

`make_levels.py` packs the game files into `levels.bin`, the raw image flashed into
the `levels` partition:

```
python make_levels.py build/levels.bin TITLE.PKD GYM.PKD LEVEL1.PKD LEVEL2.PKD TRACKS.AD4
```

The format is intentionally the simplest thing that can be consumed *in place* through
`esp_partition_mmap` — a read-only table plus aligned blobs:

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 4 | magic | `0x4C564C4F`, little-endian — reads as ASCII `"OLVL"` |
| `0x04` | 4 | `count` | number of file entries (u32 LE) |
| `0x08` | 24 x count | entry table | see below |
| after table | — | file blobs | each padded to 4-byte alignment with zeros |

Each 24-byte entry:

| Offset in entry | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 16 | `name[16]` | basename incl. extension, NUL-padded ASCII; max 15 chars (enforced by the packer) |
| `0x10` | 4 | `offset` | u32 LE, **from the start of the partition**, always 4-byte aligned |
| `0x14` | 4 | `size` | u32 LE, exact file size in bytes |

Why 4-byte alignment is load-bearing: the engine consumes `.PKD` levels by casting
structs directly over the data (little-endian 32-bit layout — the GBA files work
unmodified on Xtensa), and `TRACKS.AD4` is read straight from the mapped pointer by the
mixer. Unaligned `uint32` fields would fault or take the slow path; the packer
guarantees `base + entry.offset` is always safely castable. The header is 8 + 24n bytes
(already a multiple of 4); the packer still rounds defensively.

The consumer side (`game_main.cpp`) is symmetric and tiny — mount checks the magic,
`levelsFind(name)` does a linear `strcmp` over the table, and:

- `TRACKS.AD4` is used **directly from the mmap pointer** (music access is sequential;
  sequential flash-through-cache reads are cheap),
- level `.PKD`s are **memcpy'd from the mapped flash into PSRAM** at load (16 ms for
  the 306 KB `TITLE.PKD`, measured), because the engine mutates the level image in
  place and PSRAM serves the renderer's random texture reads 3-4x faster than flash.

The packer prints a manifest (name, offset, size per file) — keep that output; it is
the ground truth of what a given `levels.bin` contains.

## 7. flash.ps1 — offsets and usage

```
powershell -ExecutionPolicy Bypass -File flash.ps1 [COMx] [-Levels]
```

The script uses the venv's Python (`...\Scripts\python.exe -m esptool`) at 921600 baud;
with no port argument, esptool auto-detects. It always writes the three app images, and
only builds + writes the levels image when `-Levels` is passed:

| Offset | Image | When |
|---|---|---|
| `0x0` | `build\bootloader\bootloader.bin` | always |
| `0x8000` | `build\partition_table\partition-table.bin` | always |
| `0x10000` | `build\openlara_esp32s3.bin` | always |
| `0x310000` | `build\levels.bin` (built on the fly by `make_levels.py` from `src/platform/gba/data/`) | `-Levels` only |

The split exists because the payloads are wildly asymmetric: the app takes seconds to
flash, while the 9.76 MB levels image takes ~84 s at 921600 baud — and it only changes
when the level set changes. Day-to-day iteration is `build.ps1` + `flash.ps1` (app
only); `-Levels` is a once-per-level-set operation.

The `0x310000` in the script is duplicated from `partitions.csv` by hand (a comment
marks it: "keep in sync"). If you ever move the `levels` partition, both places must
change together.

## 8. Serial: monitoring and playing

The board talks over its USB-Serial-JTAG console at 115200. Two equivalent monitors:

```
# IDF's monitor (auto-decodes panics/backtraces):
C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe -m esp_idf_monitor --port COM6

# plain terminal (pyserial ships in the same venv):
C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe -m serial.tools.miniterm COM6 115200
```

What you will see at INFO level: the levels-partition mount ("levels: 5 files
mounted"), per-level load timing ("TITLE.PKD: 306 KB -> PSRAM in 16 ms"), free-heap
reports after init (33 KB internal free after full game init, largest block 17 KB),
and a `fps=NN` line every second (62-63 on the title screen, 48-50 in GYM).

The console is also an **input device**: `game_main.cpp` reads the same USB-Serial-JTAG
channel and maps characters to game keys (`w/s/a/d` = d-pad, `x` = A, `z` = B, `q` = L,
`e` = R, Enter = START, space = SELECT; each received char holds its key for 8 frames,
since a serial stream has no key-up events). You can play the game from the monitor
window with no buttons soldered.

One operational gotcha: **the COM port is exclusive.** esptool cannot open the port
while a monitor is attached — `flash.ps1` will fail with a port-busy error until you
close miniterm/esp_idf_monitor. If a flash inexplicably fails to connect, a forgotten
monitor window is the first suspect.

## Key takeaways

- `idf.py` is optional by design: export the documented environment variables, put the
  toolchain first on `PATH`, and IDF is just a CMake project. On a messy Windows
  machine, explicit-and-boring beats convenient-and-guessing — and transplanting an
  already-proven environment (velxio's) made the first firmware build succeed.
- Two Windows-specific landmines: IDF 5.x fatals if `MSYSTEM` leaks in from MSYS2
  (scrub it), and the Python venv's *name* means nothing — this project deliberately
  runs a v5.5.4 build on a venv named `idf4.4_py3.10_env` that carries the v5.x
  requirements.
- `sdkconfig.defaults` entries with misspelled symbols are **silently ignored**, and
  the file itself is only read when `sdkconfig` doesn't exist yet. Verify the
  generated `sdkconfig` after every flag change; that is how the cache geometry
  (32 KB/8-way I-cache, 64 KB/8-way/64 B-line D-cache — not IDF defaults) was
  confirmed real.
- `CONFIG_ESP32S3_DATA_CACHE_LINE_64B` is correctness, not tuning: octal DDR PSRAM
  bursts 64 bytes, and 32-byte lines are documented to corrupt the cache.
- Levels live in a raw custom partition (`subtype 0x40` at `0x310000`) as an `OLVL`
  container: 8-byte header, 24-byte entries (`name[16]`, u32 offset, u32 size),
  4-byte-aligned blobs — exactly enough structure to `esp_partition_mmap` and cast
  in place, and nothing more.
- Flash offsets: `0x0` bootloader, `0x8000` partition table, `0x10000` app,
  `0x310000` levels. App flashing is seconds; the 9.76 MB levels image (~84 s) is
  behind the `-Levels` switch because it rarely changes.
- The serial console is bidirectional: logs and FPS out, gameplay keys in — and it
  must be closed before flashing, because the COM port is exclusive.
