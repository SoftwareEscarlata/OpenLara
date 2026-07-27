# Flash the OpenLara firmware + level data to the Waveshare ESP32-S3-Touch-LCD-2.
# Usage:  powershell -ExecutionPolicy Bypass -File flash.ps1 [COMx] [-Levels]
#   COMx     serial port (default: auto-detect)
#   -Levels  also build + flash the levels partition (slow, only needed once)

param(
    [string]$Port = "",
    [switch]$Levels
)

$ErrorActionPreference = "Stop"
$PY = "C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe"
$proj  = $PSScriptRoot
$build = Join-Path $proj "build"
$gbaData = Join-Path $proj "..\..\gba\data"

$portArg = @()
if ($Port) { $portArg = @("--port", $Port) }

# app + bootloader + partition table
& $PY -m esptool --chip esp32s3 @portArg --baud 921600 write_flash `
    0x0      "$build\bootloader\bootloader.bin" `
    0x8000   "$build\partition_table\partition-table.bin" `
    0x10000  "$build\openlara_esp32s3.bin"
if ($LASTEXITCODE -ne 0) { throw "app flash failed" }

if ($Levels) {
    & $PY "$proj\make_levels.py" "$build\levels.bin" `
        "$gbaData\TITLE.PKD" "$gbaData\GYM.PKD" "$gbaData\LEVEL1.PKD" `
        "$gbaData\LEVEL2.PKD" "$gbaData\TRACKS.AD4"
    if ($LASTEXITCODE -ne 0) { throw "levels image build failed" }

    # levels partition offset — keep in sync with partitions.csv
    & $PY -m esptool --chip esp32s3 @portArg --baud 921600 write_flash `
        0x310000 "$build\levels.bin"
    if ($LASTEXITCODE -ne 0) { throw "levels flash failed" }
}

Write-Host "Flash OK. Monitor:  $PY -m esp_idf_monitor --port <COMx>  (or any 115200 terminal)"
