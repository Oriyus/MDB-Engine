# Draws the MDB Editor icon into Editor/resources/editor.ico: an isometric cube (three shaded faces) on a
# dark rounded tile. Each size (16, 24, 32, 48, 64, 128, 256) is drawn on its own, so small ones stay crisp,
# and stored as PNG inside the .ico. Run it again after changing the drawing; then rebuild the editor.
#   powershell -ExecutionPolicy Bypass -File Tools/generate_editor_icon.ps1
Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $PSScriptRoot '..\Editor\resources'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir 'editor.ico'

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = [float]$size

    # The tile: a dark blue-violet gradient with rounded corners (a hair of margin so edges don't clip)
    $m = [Math]::Max(0.5, $s * 0.02)
    $tile = New-RoundedRect $m $m ($s - 2 * $m) ($s - 2 * $m) ($s * 0.2)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s),
        ([System.Drawing.Color]::FromArgb(255, 38, 44, 78)), ([System.Drawing.Color]::FromArgb(255, 16, 18, 34))
    $g.FillPath($bg, $tile)
    if ($size -ge 32) {
        $rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(90, 120, 150, 255)), ([Math]::Max(1, $s / 64))
        $g.DrawPath($rim, $tile)
    }

    # The cube: centre, half-width and edge height of an isometric cube (30 degree faces)
    $cx = $s / 2; $cy = $s * 0.53
    $hw = $s * 0.30                   # half the cube's width
    $hh = $hw * 0.5774                # tan(30): the top face's half-height
    $e = $hw * 1.12                   # vertical edge length
    $top   = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $cx, ($cy - $e / 2 - $hh)), (New-Object System.Drawing.PointF ($cx + $hw), ($cy - $e / 2)),
                                        (New-Object System.Drawing.PointF $cx, ($cy - $e / 2 + $hh)), (New-Object System.Drawing.PointF ($cx - $hw), ($cy - $e / 2)))
    $left  = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF ($cx - $hw), ($cy - $e / 2)), (New-Object System.Drawing.PointF $cx, ($cy - $e / 2 + $hh)),
                                        (New-Object System.Drawing.PointF $cx, ($cy + $e / 2 + $hh)), (New-Object System.Drawing.PointF ($cx - $hw), ($cy + $e / 2)))
    $right = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $cx, ($cy - $e / 2 + $hh)), (New-Object System.Drawing.PointF ($cx + $hw), ($cy - $e / 2)),
                                        (New-Object System.Drawing.PointF ($cx + $hw), ($cy + $e / 2)), (New-Object System.Drawing.PointF $cx, ($cy + $e / 2 + $hh)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 132, 214, 255))), $top)
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 64, 132, 240))), $left)
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 34, 74, 170))), $right)

    # The top face's edge highlight and a small warm sun above the cube (the light), from 32 px up
    if ($size -ge 32) {
        $edge = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(160, 230, 245, 255)), ([Math]::Max(1, $s / 96))
        $edge.LineJoin = 'Round'
        $g.DrawPolygon($edge, $top)
        $r = $s * 0.06
        $glow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 196, 92))
        $g.FillEllipse($glow, $cx + $hw * 0.78 - $r, $cy - $e / 2 - $hh * 1.25 - $r, 2 * $r, 2 * $r)
    }
    $g.Dispose()
    return $bmp
}

# The .ico: a header, one 16-byte entry per size, then the PNG data of each
$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($sz in $sizes) {
    $bmp = Draw-Icon $sz
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }    # 0 means 256
    $w.Write([Byte]$dim); $w.Write([Byte]$dim); $w.Write([Byte]0); $w.Write([Byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
Write-Host "Wrote $out ($((Get-Item $out).Length) bytes, sizes $($sizes -join ', '))"

# A preview of the large size next to the icon (git-ignored by nothing; delete it if you like)
$preview = Draw-Icon 256
$preview.Save((Join-Path $outDir 'editor_preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()
