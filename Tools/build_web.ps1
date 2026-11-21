# Builds the web player (Game.html / Game.js / Game.wasm) with Emscripten, and optionally packages a project with it.
#
#   powershell -File Tools/build_web.ps1                                   # build build-web (configures it the first time)
#   powershell -File Tools/build_web.ps1 -Project Examples -Out build-web/dist/web   # ... and package a playable folder
#
# Emscripten: the folder in $env:MDB_EMSDK, else $env:EMSDK, else D:\emsdk, C:\emsdk or %USERPROFILE%\emsdk (install it
# with: git clone https://github.com/emscripten-core/emsdk; emsdk install 3.1.74; emsdk activate 3.1.74). The engine's
# pinned libraries (spdlog's fmt, Jolt, assimp, sol2) are known to build with 3.1.74; newer SDKs ship a newer clang
# and libc++ that reject parts of them.
# Ninja: on PATH, else the copy inside Visual Studio.
param(
    [string]$BuildDir = "",
    [string]$Project = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-web" }

function Find-Emsdk {
    $candidates = @($env:MDB_EMSDK, $env:EMSDK, "D:\emsdk", "C:\emsdk", (Join-Path $env:USERPROFILE "emsdk")) | Where-Object { $_ }
    foreach ($c in $candidates) { if (Test-Path (Join-Path $c "upstream\emscripten\emcc.py")) { return (Resolve-Path $c).Path } }
    return $null
}
$emsdk = Find-Emsdk
if (-not $emsdk) { Write-Error "Emscripten not found. Install emsdk (see the top of Tools/build_web.ps1) or set MDB_EMSDK to its folder." }
$python = Get-ChildItem (Join-Path $emsdk "python") -Filter python.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$node = Get-ChildItem (Join-Path $emsdk "node") -Filter node.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$env:EMSDK = $emsdk
$env:EM_CONFIG = Join-Path $emsdk ".emscripten"
if ($python) { $env:EMSDK_PYTHON = $python.FullName }
if ($node) { $env:EMSDK_NODE = $node.FullName; $env:PATH = "$($node.DirectoryName);$env:PATH" }
$env:PATH = "$(Join-Path $emsdk 'upstream\emscripten');$env:PATH"

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $vs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Filter ninja.exe -Recurse -Depth 9 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vs) { Write-Error "Ninja not found (install it, or the Visual Studio CMake tools)" }
    $env:PATH = "$($vs.DirectoryName);$env:PATH"
}

Write-Host "Emscripten: $emsdk"
$cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $cache)) {
    Write-Host "Configuring $BuildDir (the first build compiles the engine for the web: several minutes)"
    & emcmake.bat cmake -S $repo -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
# Build with the CMake that configured the folder (another one on the PATH would want to start over)
$cmake = "cmake"
$line = Select-String -Path $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
if ($line -and (Test-Path $line.Matches[0].Groups[1].Value)) { $cmake = $line.Matches[0].Groups[1].Value }
& $cmake --build $BuildDir --target Game
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Out) {
    if (-not $Project) { $Project = Join-Path $repo "Examples" }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package_web.ps1") `
        -Player (Join-Path $BuildDir "bin") -Project $Project -Engine (Join-Path $repo "EngineAssets") -Out $Out -Launcher (Join-Path $repo "Game\web")
    exit $LASTEXITCODE
}
