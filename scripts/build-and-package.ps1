[CmdletBinding()]
param(
    [string]$BuildDirectory = "build/windows",
    [string]$Version = "0.3.0-material-web",
    [string]$GStreamerRoot = "C:\Program Files\GStreamer\1.0\msvc_x86_64"
)

# One repeatable Windows release path: configure, build, test, then create the
# small app-local portable archive.  It intentionally does not stop a running
# OAMR instance; close it first when the linker needs to replace oamr.exe.
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$webRoot = Join-Path $projectRoot "web"
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw "Visual Studio developer shell was not found: $vsDevCmd" }
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found: $cmake" }

$command = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$cmake`" -S . -B $BuildDirectory -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release -DGSTREAMER_ROOT=`"$($GStreamerRoot -replace '\\','/')`" && `"$cmake`" --build $BuildDirectory && ctest --test-dir $BuildDirectory --output-on-failure"
Push-Location $projectRoot
try {
    Push-Location $webRoot
    try {
        & npm ci
        if ($LASTEXITCODE -ne 0) { throw "Could not install frontend build dependencies." }
        & npm run build
        if ($LASTEXITCODE -ne 0) { throw "Could not build the Vite frontend." }
    } finally {
        Pop-Location
    }
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Build or tests failed." }
    & (Join-Path $PSScriptRoot "package-portable.ps1") -BuildDirectory $BuildDirectory -Version $Version -GStreamerRoot $GStreamerRoot
    if ($LASTEXITCODE -ne 0) { throw "Portable packaging failed." }
} finally {
    Pop-Location
}
