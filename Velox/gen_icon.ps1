# Generates src\velox.ico - Windows 11-style app tile: green-to-blue gradient rounded square
# with white down / up arrows. Same geometry as DrawAppMark() in src\fluent.h.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'src\velox.ico'
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
        ([System.Drawing.Color]::FromArgb(255, 0x2E, 0xC4, 0x8F)), ([System.Drawing.Color]::FromArgb(255, 0x00, 0x78, 0xD4))
    $g.FillPath($lg, $bg)

    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single][math]::Max(1.3, $w * 0.085))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
    function P($fx, $fy) { New-Object System.Drawing.PointF ([single]($x + $w * $fx)), ([single]($y + $w * $fy)) }
    $g.DrawLine($pen, (P 0.38 0.26), (P 0.38 0.74)); $g.DrawLines($pen, [System.Drawing.PointF[]]@((P 0.27 0.62), (P 0.38 0.74), (P 0.49 0.62)))
    $g.DrawLine($pen, (P 0.62 0.74), (P 0.62 0.26)); $g.DrawLines($pen, [System.Drawing.PointF[]]@((P 0.51 0.38), (P 0.62 0.26), (P 0.73 0.38)))
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
