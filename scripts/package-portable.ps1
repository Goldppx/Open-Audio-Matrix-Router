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
$gstScanner = Join-Path $GStreamerRoot "libexec\gstreamer-1.0\gst-plugin-scanner.exe"
$cache = Join-Path $buildRoot "CMakeCache.txt"

if (-not (Test-Path -LiteralPath $exe)) {
    throw "OAMR executable not found: $exe. Build the project first."
}
if (-not (Test-Path -LiteralPath $cache) -or -not ((Get-Content -LiteralPath $cache -Raw) -match 'CMAKE_BUILD_TYPE:STRING=Release')) {
    throw "Portable packages must use a Release build. Configure CMake with -DCMAKE_BUILD_TYPE=Release."
}
if (-not (Test-Path -LiteralPath (Join-Path $gstBin "gstreamer-1.0-0.dll")) -or -not (Test-Path -LiteralPath $gstPlugins) -or -not (Test-Path -LiteralPath $gstScanner)) {
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
    $runtimeBin = Join-Path $stage "runtime\bin"
    $runtimePlugins = Join-Path $stage "runtime\lib\gstreamer-1.0"
    $runtimeScanner = Join-Path $stage "runtime\libexec\gstreamer-1.0"
    New-Item -ItemType Directory -Path $runtimeBin, $runtimePlugins, $runtimeScanner | Out-Null

    # OAMR's runtime closure, deliberately limited to local audio, RTP/UDP,
    # Opus and their transitive GStreamer/GLib DLLs. Update this list whenever
    # a new backend or codec is added; do not copy the whole SDK/runtime tree.
    $runtimeDlls = @(
        "gstreamer-1.0-0.dll", "gstbase-1.0-0.dll", "gstaudio-1.0-0.dll",
        "gstpbutils-1.0-0.dll", "gsttag-1.0-0.dll", "gstvideo-1.0-0.dll",
        "gstrtp-1.0-0.dll", "gstnet-1.0-0.dll", "gio-2.0-0.dll",
        "gobject-2.0-0.dll", "gmodule-2.0-0.dll", "glib-2.0-0.dll",
        "orc-0.4-0.dll", "opus-0.dll", "intl-8.dll", "z-1.dll",
        "ffi-7.dll", "pcre2-8-0.dll"
    )
    foreach ($name in $runtimeDlls) {
        $source = Join-Path $gstBin $name
        if (-not (Test-Path -LiteralPath $source)) { throw "Required GStreamer runtime DLL missing: $source" }
        Copy-Item -LiteralPath $source -Destination $runtimeBin
    }
    foreach ($name in @(
        "gstwasapi2.dll", "gstaudioconvert.dll", "gstaudioresample.dll",
        "gstaudiomixer.dll", "gstopus.dll", "gstrtp.dll",
        "gstrtpmanager.dll", "gstudp.dll", "gstautodetect.dll"
    )) {
        $source = Join-Path $gstPlugins $name
        if (-not (Test-Path -LiteralPath $source)) { throw "Required GStreamer plugin missing: $source" }
        Copy-Item -LiteralPath $source -Destination $runtimePlugins
    }
    Copy-Item -LiteralPath $gstScanner -Destination $runtimeScanner

    @'
@echo off
setlocal
set "OAMR_ROOT=%~dp0"
set "PATH=%OAMR_ROOT%runtime\bin;%PATH%"
set "GST_PLUGIN_PATH_1_0=%OAMR_ROOT%runtime\lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH_1_0=%OAMR_ROOT%runtime\lib\gstreamer-1.0"
set "GST_PLUGIN_SCANNER=%OAMR_ROOT%runtime\libexec\gstreamer-1.0\gst-plugin-scanner.exe"
chcp 65001 >nul
"%OAMR_ROOT%oamr.exe" %*
endlocal
'@ | Set-Content -LiteralPath (Join-Path $stage "run-oamr.cmd") -Encoding ascii

    New-Item -ItemType Directory -Path (Split-Path -Parent $zip) -Force | Out-Null
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "Created portable package: $zip"
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
