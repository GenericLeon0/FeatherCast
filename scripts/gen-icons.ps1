# Generates the LeanCast icon (build/icon.ico, build/icon.png, build/tray.png)
# from pure code – no external assets required.
Add-Type -AssemblyName System.Drawing

$buildDir = Join-Path $PSScriptRoot "..\build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function New-RoundedPath([int]$w, [int]$h, [int]$r) {
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $r * 2
  $path.AddArc(0, 0, $d, $d, 180, 90)
  $path.AddArc($w - $d, 0, $d, $d, 270, 90)
  $path.AddArc($w - $d, $h - $d, $d, $d, 0, 90)
  $path.AddArc(0, $h - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  return $path
}

function New-LeanBitmap([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  $radius = [int]($size * 0.22)
  $path = New-RoundedPath $size $size $radius

  $c1 = [System.Drawing.Color]::FromArgb(255, 91, 108, 255)   # Indigo
  $c2 = [System.Drawing.Color]::FromArgb(255, 124, 58, 237)   # Violet
  $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45)
  $g.FillPath($brush, $path)

  $fontSize = [single]($size * 0.58)
  $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $sf = New-Object System.Drawing.StringFormat
  $sf.Alignment = [System.Drawing.StringAlignment]::Center
  $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
  $textRect = New-Object System.Drawing.RectangleF(0, [single](-$size*0.04), $size, $size)
  $g.DrawString("L", $font, [System.Drawing.Brushes]::White, $textRect, $sf)

  $g.Dispose()
  return $bmp
}

# 256px PNG (for ICO and window icon)
$big = New-LeanBitmap 256
$pngPath = Join-Path $buildDir "icon.png"
$big.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)

# PNG bytes for the ICO container
$ms = New-Object System.IO.MemoryStream
$big.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$pngBytes = $ms.ToArray()
$ms.Dispose()

# ICO with embedded PNG (Vista+ supports PNG-in-ICO)
$icoPath = Join-Path $buildDir "icon.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)      # reserved
$bw.Write([uint16]1)      # type = icon
$bw.Write([uint16]1)      # image count
$bw.Write([byte]0)        # width 0 => 256
$bw.Write([byte]0)        # height 0 => 256
$bw.Write([byte]0)        # colors
$bw.Write([byte]0)        # reserved
$bw.Write([uint16]1)      # planes
$bw.Write([uint16]32)     # bits per pixel
$bw.Write([uint32]$pngBytes.Length)
$bw.Write([uint32]22)     # offset (6 + 16)
$bw.Write($pngBytes)
$bw.Flush(); $bw.Dispose(); $fs.Dispose()

# 32px tray PNG
$tray = New-LeanBitmap 32
$trayPath = Join-Path $buildDir "tray.png"
$tray.Save($trayPath, [System.Drawing.Imaging.ImageFormat]::Png)

$big.Dispose(); $tray.Dispose()
Write-Host "Icons generated in $buildDir : icon.ico, icon.png, tray.png"
