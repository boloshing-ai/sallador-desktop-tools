# Generates src\mensura.ico - Windows 11-style app tile: violet-to-blue gradient rounded square
# with a white gauge (arc + needle). Same geometry as DrawAppMark() in src\fluent.h.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'src\mensura.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Add-RoundRect($p, [single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
}

$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'Half'
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = if ($s -le 24) { 0.5 } else { $s * 0.04 }
    $x = $pad; $y = $pad; $w = $s - 2 * $pad
    $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-RoundRect $bg $x $y $w $w ($w * 0.23)
    $lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF ($x - 1), ($y - 1)),
        (New-Object System.Drawing.PointF ($x + $w + 1), ($y + $w + 1)),
        ([System.Drawing.Color]::FromArgb(255, 0x9B, 0x6B, 0xF7)), ([System.Drawing.Color]::FromArgb(255, 0x25, 0x5F, 0xE0))
    $g.FillPath($lg, $bg)

    $cx = $x + $w * 0.5; $cy = $y + $w * 0.64; $r = $w * 0.30
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single][math]::Max(1.4, $w * 0.09))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawArc($pen, [single]($cx - $r), [single]($cy - $r), [single](2 * $r), [single](2 * $r), 180, 180)
    $a = -0.87; $L = $w * 0.22
    $g.DrawLine($pen, [single]$cx, [single]$cy, [single]($cx + $L * [math]::Cos($a)), [single]($cy + $L * [math]::Sin($a)))
    $dot = [single][math]::Max(1.2, $w * 0.07)
    $g.FillEllipse([System.Drawing.Brushes]::White, [single]($cx - $dot), [single]($cy - $dot), [single](2 * $dot), [single](2 * $dot))
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose()
}

$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
Write-Host "Icon written: $out"
