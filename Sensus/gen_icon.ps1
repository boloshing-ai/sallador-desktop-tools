# Generates src\sensus.ico - Windows 11-style app tile: orange-to-red gradient rounded square
# with a white thermometer. Same geometry as DrawAppMark() in src\fluent.h.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'src\sensus.ico'
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
        ([System.Drawing.Color]::FromArgb(255, 0xFF, 0x9A, 0x3D)), ([System.Drawing.Color]::FromArgb(255, 0xE0, 0x3E, 0x52))
    $g.FillPath($lg, $bg)

    $wb = [System.Drawing.Brushes]::White
    $stem = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-RoundRect $stem ([single]($x + $w * 0.40)) ([single]($y + $w * 0.17)) ([single]($w * 0.14)) ([single]($w * 0.52)) ([single]($w * 0.07))
    $g.FillPath($wb, $stem)
    $r = $w * 0.14; $g.FillEllipse($wb, [single]($x + $w * 0.47 - $r), [single]($y + $w * 0.70 - $r), [single](2 * $r), [single](2 * $r))
    if ($s -ge 24) {
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single][math]::Max(1.2, $w * 0.07))
        $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
        for ($i = 0; $i -lt 3; $i++) { $ty = [single]($y + $w * (0.27 + 0.11 * $i)); $x2 = if ($i -eq 1) { 0.76 } else { 0.71 }; $g.DrawLine($pen, [single]($x + $w * 0.64), $ty, [single]($x + $w * $x2), $ty) }
    }
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
