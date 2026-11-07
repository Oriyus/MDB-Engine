# Draws the example images into Assets/Textures (PNG with transparency): the UI coin icon, and a
# fence (Cutout) and a stained-glass window (Blend) for see-through surfaces.
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

# ---- fence.png: wooden pickets and two rails; the gaps are fully transparent (for the Cutout alpha mode)
$bmp, $g = New-Canvas 256 256
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$wood = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 150, 104, 62))
$dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 110, 74, 42))
foreach ($x in 8, 72, 136, 200) {
    $g.FillRectangle($wood, $x, 26, 48, 230)
    $tip = [System.Drawing.Point[]]@((New-Object System.Drawing.Point $x, 26), (New-Object System.Drawing.Point ($x + 24), 0), (New-Object System.Drawing.Point ($x + 48), 26))
    $g.FillPolygon($wood, $tip)
    $g.FillRectangle($dark, $x + 44, 26, 4, 230)                # a shaded edge
}
$g.FillRectangle($dark, 0, 70, 256, 26)
$g.FillRectangle($dark, 0, 186, 256, 26)
$bmp.Save((Join-Path $out 'fence.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "fence.png"

# ---- window.png: stained glass, coloured half-transparent panes in an opaque lead frame (for Blend)
$bmp, $g = New-Canvas 256 256
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$panes = @(
    @(255, 70, 70), @(255, 200, 60), @(80, 160, 255),
    @(90, 210, 120), @(240, 240, 255), @(200, 90, 230),
    @(255, 140, 60), @(80, 220, 220), @(255, 90, 160))
for ($i = 0; $i -lt 9; $i++) {
    $c = $panes[$i]
    $b = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, $c[0], $c[1], $c[2]))
    $g.FillRectangle($b, ($i % 3) * 85, [math]::Floor($i / 3) * 85, 86, 86)
}
$lead = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 40, 40, 46))
foreach ($p in 0, 82, 167, 248) { $g.FillRectangle($lead, $p, 0, 8, 256); $g.FillRectangle($lead, 0, $p, 256, 8) }
$bmp.Save((Join-Path $out 'window.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "window.png"
