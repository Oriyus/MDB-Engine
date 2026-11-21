# Serves this folder on http://127.0.0.1:<port>/ and opens the game in the default browser (browsers don't run
# WebAssembly games from file:// pages). Only this computer can connect. Close the window to stop it.
param([int]$Port = 8080, [switch]$NoBrowser)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path $PSScriptRoot).Path

$listener = $null
for ($p = $Port; $p -lt $Port + 50; $p++) {
    try { $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p); $listener.Start(); break }
    catch { $listener = $null }
}
if (-not $listener) { Write-Error "No free port from $Port to $($Port + 49)" }
$url = "http://127.0.0.1:$p/"
$host.UI.RawUI.WindowTitle = "Game server - $url"
Write-Host "Serving $root"
Write-Host "Playing at $url  (close this window to stop)"
if (-not $NoBrowser) { Start-Process $url }

$types = @{
    ".html" = "text/html; charset=utf-8"; ".js" = "text/javascript"; ".wasm" = "application/wasm"
    ".json" = "application/json"; ".pak" = "application/octet-stream"; ".png" = "image/png"; ".ico" = "image/x-icon"
    ".txt" = "text/plain; charset=utf-8"; ".css" = "text/css"
}
function Send($stream, [int]$code, [string]$reason, [string]$type, [long]$length, [bool]$head) {
    $h = "HTTP/1.1 $code $reason`r`nContent-Type: $type`r`nContent-Length: $length`r`nCache-Control: no-cache`r`nConnection: close`r`n`r`n"
    $b = [System.Text.Encoding]::ASCII.GetBytes($h)
    $stream.Write($b, 0, $b.Length)
}

while ($true) {
    $client = $listener.AcceptTcpClient()
    try {
        $client.ReceiveTimeout = 5000
        $stream = $client.GetStream()
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
        $request = $reader.ReadLine()
        while ($true) { $line = $reader.ReadLine(); if ($null -eq $line -or $line -eq "") { break } }
        if (-not $request) { continue }
        $parts = $request.Split(' ')
        $method = $parts[0]
        $path = [System.Uri]::UnescapeDataString(($parts[1] -split '\?')[0])
        if ($path -eq "/") { $path = "/index.html" }
        $file = [System.IO.Path]::GetFullPath((Join-Path $root $path.TrimStart('/')))
        if (($method -ne "GET" -and $method -ne "HEAD") -or -not $file.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $file -PathType Leaf)) {
            $msg = [System.Text.Encoding]::ASCII.GetBytes("Not found")
            Send $stream 404 "Not Found" "text/plain" $msg.Length ($method -eq "HEAD")
            if ($method -ne "HEAD") { $stream.Write($msg, 0, $msg.Length) }
            continue
        }
        $ext = [System.IO.Path]::GetExtension($file).ToLowerInvariant()
        $type = if ($types.ContainsKey($ext)) { $types[$ext] } else { "application/octet-stream" }
        $fs = [System.IO.File]::OpenRead($file)
        try {
            Send $stream 200 "OK" $type $fs.Length ($method -eq "HEAD")
            if ($method -eq "GET") { $fs.CopyTo($stream, 1MB) }
        }
        finally { $fs.Dispose() }
        Write-Host "$method $path"
    }
    catch { }   # the browser closed the connection early: nothing to do
    finally { $client.Close() }
}
