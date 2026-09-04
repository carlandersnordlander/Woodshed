# Builds the executable's .ico from the mark's own geometry.
#
# The small sizes get the single-ring drawing and the large ones the three-ring drawing, which is
# what the two files are for: three rings at sixteen pixels is three grey lines.
param([string]$OutPath)

Add-Type -AssemblyName System.Drawing

$wood    = [System.Drawing.Color]::FromArgb(255, 245, 241, 234)
$accent  = [System.Drawing.Color]::FromArgb(255, 232, 148, 58)
$ground  = [System.Drawing.Color]::FromArgb(255, 20, 16, 13)

# radius, width, isAccent - fractions of the mark's radius, read off the drawings
$full = @(
  @{ r = 0.93966; w = 0.12069; a = $false },
  @{ r = 0.67241; w = 0.13793; a = $true  },
  @{ r = 0.37931; w = 0.15517; a = $false }
)
$small = @( @{ r = 0.8427; w = 0.31461; a = $false } )

function New-MarkBitmap([int]$side) {
  $bmp = New-Object System.Drawing.Bitmap($side, $side, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  # The dark rounded square the app icon sits on, so the cream is visible on any background.
  $radius = $side * 0.22
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $radius * 2
  $path.AddArc(0, 0, $d, $d, 180, 90)
  $path.AddArc($side - $d, 0, $d, $d, 270, 90)
  $path.AddArc($side - $d, $side - $d, $d, $d, 0, 90)
  $path.AddArc(0, $side - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  $brush = New-Object System.Drawing.SolidBrush($ground)
  $g.FillPath($brush, $path)

  # The mark, inset so it breathes inside the square.
  $useSmall = $side -le 32
  $rings = if ($useSmall) { $small } else { $full }
  $notch = if ($useSmall) { 0.1910 } else { 0.095 }
  $heart = if ($useSmall) { 0.33708 } else { 0.13793 }

  $R = $side * 0.5 * 0.72
  $cx = $side * 0.5
  $cy = $side * 0.5

  foreach ($ring in $rings) {
    $rr = $R * $ring.r
    $pw = [Math]::Max(1.0, $R * $ring.w)
    # The notch is a fixed width across, so its angle opens out as the rings get smaller - which is
    # what makes it read as one cut through all three rather than as a wedge.
    $half = [Math]::Asin([Math]::Min(1.0, $notch * $R / $rr)) * 180.0 / [Math]::PI
    $pen = New-Object System.Drawing.Pen(($(if ($ring.a) { $accent } else { $wood })), $pw)
    $g.DrawArc($pen, ($cx - $rr), ($cy - $rr), ($rr * 2), ($rr * 2), (-22 + $half), (360 - 2 * $half))
    $pen.Dispose()
  }

  $hr = $R * $heart
  $hb = New-Object System.Drawing.SolidBrush($wood)
  $g.FillEllipse($hb, ($cx - $hr), ($cy - $hr), ($hr * 2), ($hr * 2))
  $hb.Dispose()

  $brush.Dispose(); $path.Dispose(); $g.Dispose()
  return $bmp
}

$sides = @(16, 24, 32, 48, 64, 128, 256)
$pngs = @()
foreach ($side in $sides) {
  $bmp = New-MarkBitmap $side
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $pngs += ,($ms.ToArray())
  $ms.Dispose(); $bmp.Dispose()
}

# ICONDIR, then one ICONDIRENTRY per image, then the PNG payloads. PNG-in-ICO is understood from
# Vista onwards, and keeps a 256px entry from being a megabyte of raw bitmap.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sides.Count)

$offset = 6 + 16 * $sides.Count
for ($i = 0; $i -lt $sides.Count; $i++) {
  $side = $sides[$i]
  $w.Write([Byte]($(if ($side -ge 256) { 0 } else { $side })))
  $w.Write([Byte]($(if ($side -ge 256) { 0 } else { $side })))
  $w.Write([Byte]0)            # palette size: none, it is 32-bit
  $w.Write([Byte]0)            # reserved
  $w.Write([UInt16]1)          # colour planes
  $w.Write([UInt16]32)         # bits per pixel
  $w.Write([UInt32]$pngs[$i].Length)
  $w.Write([UInt32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($png in $pngs) { $w.Write($png) }
$w.Flush()

[System.IO.File]::WriteAllBytes($OutPath, $out.ToArray())
$w.Dispose(); $out.Dispose()
"wrote $OutPath, $((Get-Item $OutPath).Length) bytes, sizes: $($sides -join ', ')"
