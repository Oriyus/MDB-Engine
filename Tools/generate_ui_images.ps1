# Draws the example UI images into Assets/Textures (PNG with transparency).
#   powershell -ExecutionPolicy Bypass -File Tools/generate_ui_images.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\Assets\Textures'
New-Item -ItemType Directory -Force $out | Out-Null

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    return $bmp, $g
}

# ---- coin_icon.png: a gold coin with a rim, an inner ring and a shine
$bmp, $g = New-Canvas 128 128
$rim = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point 128, 128), ([System.Drawing.Color]::FromArgb(255, 255, 214, 90)), ([System.Drawing.Color]::FromArgb(255, 176, 112, 20))
$g.FillEllipse($rim, 4, 4, 120, 120)
$face = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 128, 128), (New-Object System.Drawing.Point 0, 0), ([System.Drawing.Color]::FromArgb(255, 255, 220, 100)), ([System.Drawing.Color]::FromArgb(255, 214, 150, 30))
$g.FillEllipse($face, 18, 18, 92, 92)
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 150, 95, 15)), 5
$g.DrawEllipse($pen, 30, 30, 68, 68)
$shine = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(130, 255, 255, 235))
$g.FillEllipse($shine, 34, 22, 40, 22)
$bmp.Save((Join-Path $out 'coin_icon.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "coin_icon.png"
