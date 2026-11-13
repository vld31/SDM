# Builds the demo target (RP2040 / Pico / PicoBricks) with the VS Code pico-sdk toolchain.
param([switch]$Clean)
$ErrorActionPreference = "Stop"
$home_ = $env:USERPROFILE
$sdkVer = "2.3.1"
$tcVer  = "14_2_Rel1"
$env:PICO_SDK_PATH       = "$home_\.pico-sdk\sdk\$sdkVer"
$env:PICO_TOOLCHAIN_PATH = "$home_\.pico-sdk\toolchain\$tcVer"
$cmake = "$home_\.pico-sdk\cmake\v3.31.5\bin\cmake.exe"
$ninja = "$home_\.pico-sdk\ninja\v1.12.1\ninja.exe"
$env:Path = "$env:PICO_TOOLCHAIN_PATH\bin;$home_\.pico-sdk\ninja\v1.12.1;$env:Path"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
if (-not (Test-Path $build)) { New-Item -ItemType Directory $build | Out-Null }
& $cmake -S $root -B $build -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DPICO_SDK_PATH="$env:PICO_SDK_PATH" `
    -DPICO_TOOLCHAIN_PATH="$env:PICO_TOOLCHAIN_PATH" -Dpioasm_DIR="$home_\.pico-sdk\tools\$sdkVer\pioasm" `
    -Dpicotool_DIR="$home_\.pico-sdk\picotool\$sdkVer\picotool" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ninja -C $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "UF2: $build\ubb_target_demo.uf2   ELF: $build\ubb_target_demo.elf"
