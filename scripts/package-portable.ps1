[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-portable-release",
    [string]$GStreamerRoot = "C:\Program Files\GStreamer\1.0\msvc_x86_64",
    [string]$VCRuntimeDirectory = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT",
    [string]$OutputDirectory = "dist",
    [string]$Version = "dev"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot $BuildDirectory
$exe = Join-Path $buildRoot "oamr.exe"
$gstBin = Join-Path $GStreamerRoot "bin"
$gstPlugins = Join-Path $GStreamerRoot "lib\gstreamer-1.0"
$cache = Join-Path $buildRoot "CMakeCache.txt"

if (-not (Test-Path -LiteralPath $exe)) {
    throw "OAMR executable not found: $exe. Build the project first."
}
if (-not (Test-Path -LiteralPath $cache) -or -not ((Get-Content -LiteralPath $cache -Raw) -match 'CMAKE_BUILD_TYPE:STRING=Release')) {
    throw "Portable packages must use a Release build. Configure CMake with -DCMAKE_BUILD_TYPE=Release."
}
if (-not (Test-Path -LiteralPath (Join-Path $gstBin "gstreamer-1.0-0.dll")) -or -not (Test-Path -LiteralPath $gstPlugins)) {
    throw "A GStreamer MSVC runtime was not found under: $GStreamerRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $VCRuntimeDirectory "vcruntime140.dll"))) {
    throw "The MSVC x64 redistributable files were not found: $VCRuntimeDirectory"
}

$stage = Join-Path $projectRoot (".portable-stage-" + [guid]::NewGuid().ToString("N"))
$zip = Join-Path $projectRoot (Join-Path $OutputDirectory ("OAMR-" + $Version + "-windows-x64-portable.zip"))
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "oamr.exe")
    Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination (Join-Path $stage "README.md")
    # App-local deployment: Windows resolves these official MSVC Redist DLLs
    # next to oamr.exe, so users do not need to install VC++ separately.
    Get-ChildItem -LiteralPath $VCRuntimeDirectory -Filter "*.dll" | Copy-Item -Destination $stage
    New-Item -ItemType Directory -Path (Join-Path $stage "runtime") | Out-Null

    # Copy the complete runtime trees used by the official GStreamer MSVC
    # package. Keeping all plugins avoids fragile hand-maintained DLL lists.
    foreach ($directory in @("bin", "lib", "libexec", "share", "etc")) {
        $source = Join-Path $GStreamerRoot $directory
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $stage "runtime") -Recurse
        }
    }

    @'
@echo off
setlocal
set "OAMR_ROOT=%~dp0"
set "PATH=%OAMR_ROOT%runtime\bin;%PATH%"
set "GST_PLUGIN_PATH_1_0=%OAMR_ROOT%runtime\lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH_1_0=%OAMR_ROOT%runtime\lib\gstreamer-1.0"
chcp 65001 >nul
"%OAMR_ROOT%oamr.exe" %*
endlocal
'@ | Set-Content -LiteralPath (Join-Path $stage "run-oamr.cmd") -Encoding ascii

    New-Item -ItemType Directory -Path (Split-Path -Parent $zip) -Force | Out-Null
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    # Full GStreamer trees are already mostly compressed binaries. Storing
    # them avoids multi-minute packaging runs while still producing a normal
    # portable ZIP that Windows Explorer can extract.
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel NoCompression
    Write-Host "Created portable package: $zip"
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
