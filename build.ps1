param(
  [string]$Toolchain = "",
  [switch]$WithCli
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
Write-Host "BUILD_ROOT=$PSScriptRoot"

# Locate the MinGW-w64 toolchain: -Toolchain, then W64DEVKIT, then PATH.
if (-not $Toolchain -and $env:W64DEVKIT) { $Toolchain = $env:W64DEVKIT }
if (-not $Toolchain) {
  $command = Get-Command g++.exe -ErrorAction SilentlyContinue
  if ($command) { $Toolchain = Split-Path (Split-Path $command.Source -Parent) -Parent }
}
if (-not $Toolchain) {
  throw "MinGW-w64 toolchain not found. Pass -Toolchain <dir>, set W64DEVKIT, or put g++.exe on PATH."
}

$Gpp = Join-Path $Toolchain "bin\g++.exe"
$Windres = Join-Path $Toolchain "bin\windres.exe"
$Objcopy = Join-Path $Toolchain "bin\objcopy.exe"
if (-not (Test-Path $Gpp)) { throw "g++.exe not found: $Gpp" }
if (-not (Test-Path $Windres)) { throw "windres.exe not found: $Windres" }
if (-not (Test-Path $Objcopy)) { throw "objcopy.exe not found: $Objcopy" }
New-Item -ItemType Directory -Force -Path ".tmp", "dist" | Out-Null

# windres must run from the project root so app.manifest is picked up here.
& $Windres -I $PSScriptRoot -O coff -o (Join-Path $PSScriptRoot ".tmp\resources.o") "resources.rc"
if (-not $?) { throw "Build step failed." }
$AnchoredResourceObject = Join-Path $PSScriptRoot ".tmp\resources-anchored.o"
& $Objcopy --add-symbol "ZipRepairResourceAnchor=.rsrc:0,global" ".tmp\resources.o" ".tmp\resources-anchored.o"
if (-not $?) { throw "Resource anchoring failed." }
Write-Host "RESOURCE_OK"

$Common = @("-std=c++17", "-Oz", "-DUNICODE", "-D_UNICODE", "-DNDEBUG", "-static", "-static-libgcc", "-static-libstdc++", "-ffunction-sections", "-fdata-sections", "-fno-exceptions", "-fno-rtti", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wl,--gc-sections")
if ($WithCli) {
  & $Gpp @Common -municode -mconsole "repair_cli.cpp" "repair_core.cpp" -o "dist\ZipRepairDesk-CLI.exe" -s -luser32
  if (-not $?) { throw "Build step failed." }
}

$Libraries = @("-lgdiplus", "-lcomctl32", "-lwindowscodecs", "-ldwmapi", "-lshell32", "-lole32", "-luuid", "-lshlwapi", "-lshcore", "-lgdi32", "-luser32", "-ladvapi32", "-luxtheme", "-lmsimg32", "-lcomdlg32")
& $Gpp @Common -municode -mwindows "main.cpp" "repair_core.cpp" $AnchoredResourceObject "-Wl,--undefined=ZipRepairResourceAnchor" -o "dist\ZipRepairDesk.exe" -s @Libraries
if (-not $?) { throw "Build step failed." }

$outputs = @("dist\ZipRepairDesk.exe")
if ($WithCli) { $outputs += "dist\ZipRepairDesk-CLI.exe" }
Get-Item $outputs | Select-Object FullName, Length
