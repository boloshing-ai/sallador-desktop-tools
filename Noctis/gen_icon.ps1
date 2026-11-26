# Generates src\noctis.ico — Windows 11-style app tile: blue gradient rounded square, white crescent.
# Same geometry as DrawAppMark() in src\fluent.h.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'src\noctis.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Add-RoundRect($p, [single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
}
function Norm([double]$v) { (($v % 360) + 360) % 360 }
function Deg([double]$yy, [double]$xx) { [math]::Atan2($yy, $xx) * 180 / [math]::PI }
function Add-Crescent($p, [double]$x1, [double]$y1, [double]$r1, [double]$x2, [double]$y2, [double]$r2) {
    $dx = $x2 - $x1; $dy = $y2 - $y1; $d = [math]::Sqrt($dx * $dx + $dy * $dy)
    $a = ($r1 * $r1 - $r2 * $r2 + $d * $d) / (2 * $d)
    $h = [math]::Sqrt([math]::Max($r1 * $r1 - $a * $a, 0))
    $px = $x1 + $a * $dx / $d; $py = $y1 + $a * $dy / $d
    $ix1 = $px - $h * $dy / $d; $iy1 = $py + $h * $dx / $d
    $ix2 = $px + $h * $dy / $d; $iy2 = $py - $h * $dx / $d
    $a1 = Deg ($iy1 - $y1) ($ix1 - $x1); $a2 = Deg ($iy2 - $y1) ($ix2 - $x1); $phi = Deg $dy $dx
    $s1 = Norm ($a2 - $a1); $t1 = Norm ($phi - $a1)
    $sw1 = if ($t1 -lt $s1) { $s1 - 360 } else { $s1 }
    $b2 = Deg ($iy2 - $y2) ($ix2 - $x2); $b1 = Deg ($iy1 - $y2) ($ix1 - $x2); $psi = Deg (-$dy) (-$dx)
    $s2 = Norm ($b1 - $b2); $t2 = Norm ($psi - $b2)
    $sw2 = if ($t2 -lt $s2) { $s2 } else { $s2 - 360 }
    $p.AddArc([single]($x1 - $r1), [single]($y1 - $r1), [single](2 * $r1), [single](2 * $r1), [single]$a1, [single]$sw1)
    $p.AddArc([single]($x2 - $r2), [single]($y2 - $r2), [single](2 * $r2), [single](2 * $r2), [single]$b2, [single]$sw2)
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
        ([System.Drawing.Color]::FromArgb(255, 0x4C, 0xC2, 0xFF)), ([System.Drawing.Color]::FromArgb(255, 0x00, 0x5F, 0xB8))
    $g.FillPath($lg, $bg)

    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $m = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-Crescent $m ($x + $w * 0.46) ($y + $w * 0.54) ($w * 0.29) ($x + $w * 0.62) ($y + $w * 0.40) ($w * 0.25)
    $g.FillPath($white, $m)
    if ($w -ge 28) {
        $cx = $x + $w * 0.74; $cy = $y + $w * 0.30; $r = $w * 0.075
        $st = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $cx, ($cy - $r)), (New-Object System.Drawing.PointF ($cx + $r), $cy),
                                        (New-Object System.Drawing.PointF $cx, ($cy + $r)), (New-Object System.Drawing.PointF ($cx - $r), $cy))
        $g.FillPolygon($white, $st)
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
