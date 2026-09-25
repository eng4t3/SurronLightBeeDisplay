<#
.SYNOPSIS
    Local build helper for WheelieAssist (mirrors .github/workflows/release.yml).

.DESCRIPTION
    Compiles Main/Main.ino with arduino-cli using the same FQBN as CI, exports
    the binaries to .\build, and prints where the OTA .bin ended up plus the
    firmware version from Main/version.h.

.PARAMETER Upload
    After a successful build, flash the board over USB instead of just
    building. Requires -Port.

.PARAMETER Port
    Serial port to use with -Upload, e.g. COM5.

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Upload -Port COM5
#>

param(
    [switch]$Upload,
    [string]$Port
)

$ErrorActionPreference = "Stop"

$RepoRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$SketchDir  = Join-Path $RepoRoot "Main"
$VersionFile = Join-Path $SketchDir "version.h"
$BuildDir   = Join-Path $RepoRoot "build"
$Fqbn       = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc"

if ($Upload -and (-not $Port)) {
    Write-Error "Use -Port COM5 (or whichever port the board is on) together with -Upload."
    exit 1
}

# --- Locate arduino-cli: prefer the copy bundled with the Arduino IDE, else PATH. ---
$BundledCli = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$ArduinoCli = $null

if (Test-Path $BundledCli) {
    $ArduinoCli = $BundledCli
} else {
    $onPath = Get-Command "arduino-cli" -ErrorAction SilentlyContinue
    if ($onPath) {
        $ArduinoCli = $onPath.Source
    }
}

if (-not $ArduinoCli) {
    Write-Error "arduino-cli not found. Expected it at '$BundledCli' (Arduino IDE install) or on PATH."
    exit 1
}

Write-Host "Using arduino-cli: $ArduinoCli"

# --- Read FW_VERSION out of version.h. ---
if (-not (Test-Path $VersionFile)) {
    Write-Error "Could not find $VersionFile"
    exit 1
}

$VersionLine = Select-String -Path $VersionFile -Pattern '#define\s+FW_VERSION\s+"([^"]+)"'
if (-not $VersionLine) {
    Write-Error "Could not find FW_VERSION in $VersionFile"
    exit 1
}
$FwVersion = $VersionLine.Matches[0].Groups[1].Value
Write-Host "Firmware version: $FwVersion"

# --- Compile. ---
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Write-Host ""
Write-Host "Compiling $SketchDir ..."
Write-Host "FQBN: $Fqbn"
Write-Host ""

& $ArduinoCli compile --fqbn $Fqbn --export-binaries --output-dir $BuildDir $SketchDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "arduino-cli compile failed (exit code $LASTEXITCODE)."
    exit $LASTEXITCODE
}

# --- Report the files that matter. ---
$OtaBin  = Join-Path $BuildDir "Main.ino.bin"
$FullBin = Join-Path $BuildDir "Main.ino.merged.bin"

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "  Version:        $FwVersion"

if (Test-Path $OtaBin) {
    Write-Host "  OTA .bin (web updater):  $OtaBin"
} else {
    Write-Warning "Expected OTA binary not found at $OtaBin"
}

if (Test-Path $FullBin) {
    Write-Host "  Full recovery image (USB at 0x0, ERASES odometer/settings): $FullBin"
} else {
    Write-Warning "Expected merged binary not found at $FullBin"
}

# --- Optional: flash over USB. ---
if ($Upload) {
    Write-Host ""
    Write-Host "Uploading to $Port ..."
    & $ArduinoCli upload --fqbn $Fqbn --port $Port --input-dir $BuildDir $SketchDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "arduino-cli upload failed (exit code $LASTEXITCODE)."
        exit $LASTEXITCODE
    }
    Write-Host "Upload complete." -ForegroundColor Green
}
