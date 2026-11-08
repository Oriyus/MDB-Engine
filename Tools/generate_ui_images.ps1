# Draws the example images into Examples/Assets/Textures (PNG with transparency): the UI coin icon, and a
# fence (Cutout) and a stained-glass window (Blend) for see-through surfaces.
#   powershell -ExecutionPolicy Bypass -File Tools/generate_ui_images.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\Examples\Assets\Textures'
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

# ---- UI: item icons (128 x 128, in Textures/Icons) and two 9-slice frames
$icons = Join-Path $out 'Icons'
New-Item -ItemType Directory -Force $icons | Out-Null
function C([int]$a, [int]$r, [int]$gg, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $gg, $b) }
function Brush($c) { New-Object System.Drawing.SolidBrush $c }
function Pen($c, [float]$w) { $p = New-Object System.Drawing.Pen $c, $w; $p.LineJoin = 'Round'; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p }
function Pts([int[]]$xy) { $l = @(); for ($i = 0; $i -lt $xy.Length; $i += 2) { $l += New-Object System.Drawing.Point $xy[$i], $xy[$i + 1] }; [System.Drawing.Point[]]$l }
function Save-Icon($bmp, $g, [string]$name) {
    $bmp.Save((Join-Path $icons $name), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); Write-Host "Icons/$name"
}
$outline = C 255 20 18 24

# sword: a blade on the diagonal, cross guard, grip and pommel
$bmp, $g = New-Canvas 128 128
$blade = Pts @(100, 12, 116, 12, 116, 28, 52, 92, 36, 76)
$g.FillPolygon((Brush (C 255 205 214 228)), $blade); $g.DrawPolygon((Pen $outline 4), $blade)
$g.DrawLine((Pen (C 255 150 160 178) 3), 108, 20, 46, 82)
$g.DrawLine((Pen $outline 14), 30, 66, 62, 98); $g.DrawLine((Pen (C 255 214 168 60) 8), 30, 66, 62, 98)
$g.DrawLine((Pen $outline 12), 42, 86, 22, 106); $g.DrawLine((Pen (C 255 112 70 40) 7), 42, 86, 22, 106)
$g.FillEllipse((Brush (C 255 214 168 60)), 12, 102, 16, 16); $g.DrawEllipse((Pen $outline 3), 12, 102, 16, 16)
Save-Icon $bmp $g 'sword.png'

# shield: a heater shield with a boss and a band
$bmp, $g = New-Canvas 128 128
$shape = Pts @(20, 18, 108, 18, 108, 60, 64, 116, 20, 60)
$g.FillPolygon((Brush (C 255 52 92 170)), $shape)
$g.FillPolygon((Brush (C 255 74 124 210)), (Pts @(20, 18, 64, 18, 64, 116, 20, 60)))
$g.DrawLine((Pen (C 255 222 186 80) 10), 64, 22, 64, 110)
$g.DrawLine((Pen (C 255 222 186 80) 10), 24, 52, 104, 52)
$g.DrawPolygon((Pen $outline 5), $shape)
$g.FillEllipse((Brush (C 255 240 210 110)), 52, 40, 24, 24); $g.DrawEllipse((Pen $outline 3), 52, 40, 24, 24)
Save-Icon $bmp $g 'shield.png'

# potion: a round red flask with a cork and a shine
$bmp, $g = New-Canvas 128 128
$g.FillRectangle((Brush (C 255 150 200 220)), 52, 18, 24, 30); $g.DrawRectangle((Pen $outline 4), 52, 18, 24, 30)
$g.FillRectangle((Brush (C 255 150 100 60)), 50, 8, 28, 14); $g.DrawRectangle((Pen $outline 4), 50, 8, 28, 14)
$g.FillEllipse((Brush (C 255 200 30 50)), 24, 40, 80, 80)
$g.FillEllipse((Brush (C 255 236 60 80)), 30, 56, 66, 58)
$g.DrawEllipse((Pen $outline 5), 24, 40, 80, 80)
$g.FillEllipse((Brush (C 170 255 255 255)), 40, 54, 18, 26)
Save-Icon $bmp $g 'potion.png'

# gem: a faceted green gem
$bmp, $g = New-Canvas 128 128
$g.FillPolygon((Brush (C 255 40 170 110)), (Pts @(30, 44, 64, 112, 98, 44)))
$g.FillPolygon((Brush (C 255 80 220 150)), (Pts @(30, 44, 46, 20, 82, 20, 98, 44)))
$g.FillPolygon((Brush (C 255 150 250 200)), (Pts @(46, 20, 64, 44, 82, 20)))
$g.DrawLine((Pen (C 160 20 90 60) 3), 30, 44, 98, 44); $g.DrawLine((Pen (C 160 20 90 60) 3), 64, 44, 64, 112)
$g.DrawPolygon((Pen $outline 5), (Pts @(30, 44, 46, 20, 82, 20, 98, 44, 64, 112)))
Save-Icon $bmp $g 'gem.png'

# scroll: parchment with rolled ends and lines of writing
$bmp, $g = New-Canvas 128 128
$g.FillRectangle((Brush (C 255 236 214 160)), 28, 26, 72, 76); $g.DrawRectangle((Pen $outline 4), 28, 26, 72, 76)
foreach ($y in 40, 52, 64, 76, 88) { $g.DrawLine((Pen (C 255 120 90 60) 3), 38, $y, 90 - ($y % 24), $y) }
foreach ($y in 16, 96) { $g.FillEllipse((Brush (C 255 210 180 120)), 20, $y, 88, 18); $g.DrawEllipse((Pen $outline 4), 20, $y, 88, 18) }
$g.FillRectangle((Brush (C 255 180 40 40)), 84, 100, 10, 22)
Save-Icon $bmp $g 'scroll.png'

# helmet: a round helm with a visor slit and a crest
$bmp, $g = New-Canvas 128 128
$g.FillPie((Brush (C 255 150 158 172)), 22, 24, 84, 100, 180, 180)
$g.FillRectangle((Brush (C 255 150 158 172)), 22, 72, 84, 34)
$g.FillRectangle((Brush (C 255 120 128 142)), 22, 88, 84, 18)
$g.FillRectangle((Brush $outline), 34, 66, 60, 10)
$g.DrawPie((Pen $outline 5), 22, 24, 84, 100, 180, 180); $g.DrawRectangle((Pen $outline 5), 22, 74, 84, 32)
$g.FillPolygon((Brush (C 255 200 40 40)), (Pts @(58, 26, 70, 26, 74, 6, 54, 6)))
Save-Icon $bmp $g 'helmet.png'

# panel_frame.png: 96 x 96, a 20 px gold-trimmed border around a dark middle (9-slice: slice = 20)
$bmp, $g = New-Canvas 96 96
$g.FillRectangle((Brush (C 240 16 18 26)), 4, 4, 88, 88)
$g.DrawRectangle((Pen (C 255 140 104 46) 4), 4, 4, 87, 87)
$g.DrawRectangle((Pen (C 255 222 180 92) 2), 9, 9, 77, 77)
foreach ($p in @(@(4, 4), @(76, 4), @(4, 76), @(76, 76))) {
    $g.FillRectangle((Brush (C 255 222 180 92)), $p[0], $p[1], 16, 16)
    $g.FillRectangle((Brush (C 255 120 86 36)), $p[0] + 4, $p[1] + 4, 8, 8)
}
$bmp.Save((Join-Path $out 'panel_frame.png'), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
Write-Host "panel_frame.png"

# slot_frame.png: 64 x 64, a bevelled inset square (9-slice: slice = 12)
$bmp, $g = New-Canvas 64 64
$g.FillRectangle((Brush (C 255 24 26 34)), 2, 2, 60, 60)
$g.FillRectangle((Brush (C 255 14 15 20)), 8, 8, 48, 48)
$g.DrawRectangle((Pen (C 255 110 86 50) 2), 2, 2, 59, 59)
$g.DrawLine((Pen (C 255 60 62 74) 2), 8, 8, 56, 8); $g.DrawLine((Pen (C 255 60 62 74) 2), 8, 8, 8, 56)
$bmp.Save((Join-Path $out 'slot_frame.png'), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
Write-Host "slot_frame.png"
