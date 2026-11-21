# Packages a project as a web game: a folder to upload to a web host (itch.io: zip it, "HTML" kind) or to play with
# its Play.bat.
#   -Player   the folder with the web player: Game.html, Game.js, Game.wasm (build-web/bin)
#   -Project  the project folder (project.json + Assets)
#   -Engine   EngineAssets
#   -Out      the folder to fill (replaced if it holds an earlier web build)
#   -Launcher Game/web (Play.bat, serve.ps1)
# The output: index.html (the player's page), Game.js, Game.wasm, game.pak (the engine's files, the project's Assets
# without its "exclude" patterns, and project.json; format in Game/web/shell.html) and the launcher.
# The editor's File > Build Game > Web does the same (Editor/src/GameBuilder.cpp).
param(
    [Parameter(Mandatory)] [string]$Player,
    [Parameter(Mandatory)] [string]$Project,
    [Parameter(Mandatory)] [string]$Engine,
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string]$Launcher
)
$ErrorActionPreference = "Stop"
$Project = (Resolve-Path $Project).Path
$Engine = (Resolve-Path $Engine).Path
foreach ($f in "Game.html", "Game.js", "Game.wasm") {
    if (-not (Test-Path (Join-Path $Player $f))) { Write-Error "No $f in $Player (build the web player first: Tools/build_web.ps1)" }
}
$projectFile = Join-Path $Project "project.json"
if (-not (Test-Path $projectFile)) { Write-Error "$Project has no project.json" }
$settings = Get-Content $projectFile -Raw | ConvertFrom-Json
$title = if ($settings.title) { [string]$settings.title } else { "Game" }
$exclude = @()
if ($settings.exclude) { $exclude = @($settings.exclude | ForEach-Object { ([WildcardPattern]::Escape([string]$_)).Replace('`*', '*') }) }

if (Test-Path $Out) {
    if (-not (Test-Path (Join-Path $Out "game.pak"))) { Write-Error "$Out exists and is not an earlier web build; pick another folder" }
    Remove-Item $Out -Recurse -Force
}
New-Item -ItemType Directory -Force $Out | Out-Null

# The files: [package path, file on disk]
$files = New-Object System.Collections.Generic.List[object]
function Add-Tree($root, $prefix, $useExclude) {
    Get-ChildItem $root -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
        if ($useExclude) { foreach ($p in $exclude) { if ($rel -like $p) { $script:skipped++; return } } }
        $files.Add(@("$prefix$rel", $_.FullName))
    }
}
$script:skipped = 0
Add-Tree $Engine "EngineAssets/" $false
Add-Tree (Join-Path $Project "Assets") "Assets/" $true
$files.Add(@("project.json", $projectFile))

# game.pak: "MDBPAK01", uint32 index length, the JSON index [[path, offset, size], ...], then the bytes
$index = New-Object System.Text.StringBuilder
[void]$index.Append('[')
$offset = [int64]0
for ($i = 0; $i -lt $files.Count; $i++) {
    $size = (Get-Item -LiteralPath $files[$i][1]).Length
    $path = $files[$i][0].Replace('\', '\\').Replace('"', '\"')
    if ($i) { [void]$index.Append(',') }
    [void]$index.Append("[`"$path`",$offset,$size]")
    $offset += $size
}
[void]$index.Append(']')
$indexBytes = [System.Text.Encoding]::UTF8.GetBytes($index.ToString())
$pak = [System.IO.File]::Create((Join-Path $Out "game.pak"))
try {
    $magic = [System.Text.Encoding]::ASCII.GetBytes("MDBPAK01")
    $pak.Write($magic, 0, 8)
    $pak.Write([BitConverter]::GetBytes([uint32]$indexBytes.Length), 0, 4)
    $pak.Write($indexBytes, 0, $indexBytes.Length)
    foreach ($f in $files) {
        $bytes = [System.IO.File]::ReadAllBytes($f[1])
        $pak.Write($bytes, 0, $bytes.Length)
    }
}
finally { $pak.Dispose() }

# The page (titled after the game) and the player
$html = Get-Content (Join-Path $Player "Game.html") -Raw -Encoding UTF8
$safeTitle = [System.Net.WebUtility]::HtmlEncode($title)
$html = $html.Replace("<title>MDB Game</title>", "<title>$safeTitle</title>")
[System.IO.File]::WriteAllText((Join-Path $Out "index.html"), $html, (New-Object System.Text.UTF8Encoding $false))
Copy-Item (Join-Path $Player "Game.js"), (Join-Path $Player "Game.wasm") $Out
Copy-Item (Join-Path $Launcher "Play.bat"), (Join-Path $Launcher "serve.ps1") $Out

$mb = "{0:N1}" -f ($offset / 1MB)
Write-Host "Web game in $Out : $($files.Count) files, $mb MB in game.pak$(if ($script:skipped) { ", $($script:skipped) left out by exclude" })"
Write-Host "Play it with Play.bat there, or upload the folder to a web host."
