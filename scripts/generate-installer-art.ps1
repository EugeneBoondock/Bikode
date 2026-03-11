[CmdletBinding()]
param(
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $ProjectRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName PresentationCore

function New-ThemeColor {
    param(
        [int]$Red,
        [int]$Green,
        [int]$Blue,
        [int]$Alpha = 255
    )

    return [System.Drawing.Color]::FromArgb($Alpha, $Red, $Green, $Blue)
}

function New-RoundedRectanglePath {
    param(
        [System.Drawing.RectangleF]$Rectangle,
        [float]$Radius
    )

    $diameter = [Math]::Max(2.0, $Radius * 2.0)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arc = [System.Drawing.RectangleF]::new($Rectangle.X, $Rectangle.Y, $diameter, $diameter)

    $path.AddArc($arc, 180, 90)
    $arc.X = $Rectangle.Right - $diameter
    $path.AddArc($arc, 270, 90)
    $arc.Y = $Rectangle.Bottom - $diameter
    $path.AddArc($arc, 0, 90)
    $arc.X = $Rectangle.X
    $path.AddArc($arc, 90, 90)
    $path.CloseFigure()

    return $path
}

function Add-Grain {
    param(
        [System.Drawing.Graphics]$Graphics,
        [int]$Width,
        [int]$Height,
        [int]$Seed,
        [double]$Density,
        [int]$MinAlpha,
        [int]$MaxAlpha
    )

    $random = [System.Random]::new($Seed)
    $count = [Math]::Max([int]($Width * $Height * $Density), 180)

    for ($i = 0; $i -lt $count; $i++) {
        $alpha = $random.Next($MinAlpha, $MaxAlpha + 1)
        $tone = $random.Next(38, 96)
        $size = $random.Next(1, 3)
        $x = $random.Next(0, $Width)
        $y = $random.Next(0, $Height)
        $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($alpha, $tone, $tone, $tone))
        $Graphics.FillRectangle($brush, $x, $y, $size, $size)
        $brush.Dispose()
    }
}

function Add-Scanlines {
    param(
        [System.Drawing.Graphics]$Graphics,
        [int]$Width,
        [int]$Height
    )

    for ($y = 0; $y -lt $Height; $y += 3) {
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(8, 255, 255, 255))
        $Graphics.DrawLine($pen, 0, $y, $Width, $y)
        $pen.Dispose()
    }
}

function Add-Glow {
    param(
        [System.Drawing.Graphics]$Graphics,
        [float]$CenterX,
        [float]$CenterY,
        [float]$RadiusX,
        [float]$RadiusY,
        [System.Drawing.Color]$Color
    )

    for ($step = 7; $step -ge 1; $step--) {
        $mix = $step / 7.0
        $alpha = [int](18 * $mix)
        $width = $RadiusX * (0.35 + (1.0 - $mix) * 1.35)
        $height = $RadiusY * (0.35 + (1.0 - $mix) * 1.35)
        $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($alpha, $Color.R, $Color.G, $Color.B))
        $Graphics.FillEllipse($brush, $CenterX - ($width / 2.0), $CenterY - ($height / 2.0), $width, $height)
        $brush.Dispose()
    }
}

function Draw-Chip {
    param(
        [System.Drawing.Graphics]$Graphics,
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [System.Drawing.Color]$FillColor,
        [System.Drawing.Color]$BorderColor,
        [System.Drawing.Color]$TextColor,
        [string]$Text,
        [System.Drawing.Font]$Font,
        [System.Drawing.Color]$AccentColor = [System.Drawing.Color]::Empty,
        [switch]$SolidAccentBlock
    )

    $rect = [System.Drawing.RectangleF]::new($X, $Y, $Width, $Height)
    $path = New-RoundedRectanglePath -Rectangle $rect -Radius ([Math]::Min($Height / 2.0, 14.0))
    $fillBrush = New-Object System.Drawing.SolidBrush($FillColor)
    $borderPen = New-Object System.Drawing.Pen($BorderColor)

    $Graphics.FillPath($fillBrush, $path)
    $Graphics.DrawPath($borderPen, $path)

    if ($AccentColor -ne [System.Drawing.Color]::Empty) {
        if ($SolidAccentBlock) {
            $accentRect = [System.Drawing.RectangleF]::new($X, $Y, [Math]::Min(12.0, $Width * 0.18), $Height)
            $accentBrush = New-Object System.Drawing.SolidBrush($AccentColor)
            $Graphics.FillRectangle($accentBrush, $accentRect)
            $accentBrush.Dispose()
        } else {
            $accentRect = [System.Drawing.RectangleF]::new(($X + 4), ($Y + 4), 5, ($Height - 8))
            $accentBrush = New-Object System.Drawing.SolidBrush($AccentColor)
            $Graphics.FillRectangle($accentBrush, $accentRect)
            $accentBrush.Dispose()
        }
    }

    $format = New-Object System.Drawing.StringFormat
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $format.Alignment = [System.Drawing.StringAlignment]::Center

    $textInset = 0
    if ($AccentColor -ne [System.Drawing.Color]::Empty -and -not $SolidAccentBlock) {
        $textInset = 10
    }

    $textRect = [System.Drawing.RectangleF]::new(
        ($X + $textInset),
        $Y,
        ($Width - $textInset),
        $Height
    )
    $textBrush = New-Object System.Drawing.SolidBrush($TextColor)
    $Graphics.DrawString($Text, $Font, $textBrush, $textRect, $format)

    $textBrush.Dispose()
    $format.Dispose()
    $borderPen.Dispose()
    $fillBrush.Dispose()
    $path.Dispose()
}

function Save-Bitmap {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [string]$Path
    )

    $directory = Split-Path -Parent $Path
    if (-not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    if (Test-Path $Path) {
        Remove-Item $Path -Force
    }

    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($extension -eq ".png") {
        $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Bmp)
    }
}

function Get-BrandBitmap {
    param(
        [string]$ImagePath
    )

    if (-not $ImagePath -or -not (Test-Path $ImagePath)) {
        return $null
    }

    $extension = [System.IO.Path]::GetExtension($ImagePath).ToLowerInvariant()
    if ($extension -ne ".ico") {
        $image = [System.Drawing.Image]::FromFile($ImagePath)
        try {
            return [System.Drawing.Bitmap]::new($image)
        } finally {
            $image.Dispose()
        }
    }

    $stream = [System.IO.File]::OpenRead($ImagePath)
    try {
        $decoder = [System.Windows.Media.Imaging.IconBitmapDecoder]::new(
            $stream,
            [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
            [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad
        )
        $frame = $decoder.Frames | Sort-Object PixelWidth -Descending | Select-Object -First 1
        if (-not $frame) {
            return $null
        }

        $memory = New-Object System.IO.MemoryStream
        try {
            $encoder = [System.Windows.Media.Imaging.PngBitmapEncoder]::new()
            $encoder.Frames.Add($frame)
            $encoder.Save($memory)
            $memory.Position = 0

            $image = [System.Drawing.Image]::FromStream($memory)
            try {
                return [System.Drawing.Bitmap]::new($image)
            } finally {
                $image.Dispose()
            }
        } finally {
            $memory.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function New-WizardBanner {
    param(
        [string]$OutputPath,
        [string]$ProductLogoPath,
        [string]$BuilderLogoPath
    )

    $width = 760
    $height = 120
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    $bg = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $topColor = New-ThemeColor 62 62 62
    $bottomColor = New-ThemeColor 24 24 24
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush($bg, $topColor, $bottomColor, 38.0)
    $graphics.FillRectangle($gradient, $bg)
    $gradient.Dispose()

    Add-Glow -Graphics $graphics -CenterX 180 -CenterY 18 -RadiusX 320 -RadiusY 120 -Color (New-ThemeColor 210 210 210)
    Add-Glow -Graphics $graphics -CenterX 636 -CenterY 70 -RadiusX 220 -RadiusY 120 -Color (New-ThemeColor 96 96 96)

    $overlayBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(78, 14, 14, 14))
    $graphics.FillRectangle($overlayBrush, 0, 0, $width, $height)
    $overlayBrush.Dispose()

    Add-Scanlines -Graphics $graphics -Width $width -Height $height
    Add-Grain -Graphics $graphics -Width $width -Height $height -Seed 271828 -Density 0.024 -MinAlpha 12 -MaxAlpha 34

    $panelRect = [System.Drawing.RectangleF]::new(16, 12, 728, 96)
    $panelPath = New-RoundedRectanglePath -Rectangle $panelRect -Radius 14
    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(112, 18, 18, 18))
    $panelBorder = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(192, 92, 92, 92))
    $graphics.FillPath($panelBrush, $panelPath)
    $graphics.DrawPath($panelBorder, $panelPath)
    $panelBrush.Dispose()
    $panelBorder.Dispose()
    $panelPath.Dispose()

    $brandFont = New-Object System.Drawing.Font("Segoe UI Semibold", 22, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $titleFont = New-Object System.Drawing.Font("Consolas", 10.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $chipFont = New-Object System.Drawing.Font("Consolas", 8.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $builderFont = New-Object System.Drawing.Font("Segoe UI Semibold", 10, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

    $brandBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 236 236 236))
    $titleBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 188 188 188))
    $mutedBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 136 136 136))
    $builderBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 226 226 226))
    $graphics.DrawString("BIKODE", $brandFont, $brandBrush, 36, 28)
    $graphics.DrawString("AI IDE", $chipFont, $mutedBrush, 38, 56)
    $graphics.DrawString("Grey grain. Real logo. AI-first editor.", $titleFont, $titleBrush, 36, 78)

    Draw-Chip -Graphics $graphics -X 36 -Y 16 -Width 76 -Height 18 `
        -FillColor (New-ThemeColor 26 26 26 210) `
        -BorderColor (New-ThemeColor 112 112 112) `
        -TextColor (New-ThemeColor 216 216 216) `
        -Text "SETUP" `
        -Font $chipFont

    $ruleBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 210 210 210))
    $graphics.FillRectangle($ruleBrush, 36, 68, 214, 1)
    $ruleBrush.Dispose()

    $logoPanel = [System.Drawing.RectangleF]::new(600, 18, 122, 82)
    $logoPanelPath = New-RoundedRectanglePath -Rectangle $logoPanel -Radius 12
    $logoPanelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(88, 8, 8, 8))
    $logoPanelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(156, 86, 86, 86))
    $graphics.FillPath($logoPanelBrush, $logoPanelPath)
    $graphics.DrawPath($logoPanelPen, $logoPanelPath)
    $logoPanelBrush.Dispose()
    $logoPanelPen.Dispose()
    $logoPanelPath.Dispose()

    $builderPanel = [System.Drawing.RectangleF]::new(334, 66, 232, 32)
    $builderPanelPath = New-RoundedRectanglePath -Rectangle $builderPanel -Radius 10
    $builderPanelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(88, 10, 10, 10))
    $builderPanelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(132, 118, 118, 118))
    $graphics.FillPath($builderPanelBrush, $builderPanelPath)
    $graphics.DrawPath($builderPanelPen, $builderPanelPath)
    $builderPanelBrush.Dispose()
    $builderPanelPen.Dispose()
    $builderPanelPath.Dispose()

    $builderIconPanel = [System.Drawing.RectangleF]::new(341, 68, 28, 28)
    $builderIconPanelPath = New-RoundedRectanglePath -Rectangle $builderIconPanel -Radius 8
    $builderIconBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(220, 54, 54, 54))
    $builderIconPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(160, 185, 129, 38))
    $graphics.FillPath($builderIconBrush, $builderIconPanelPath)
    $graphics.DrawPath($builderIconPen, $builderIconPanelPath)
    $builderIconBrush.Dispose()
    $builderIconPen.Dispose()
    $builderIconPanelPath.Dispose()

    $graphics.DrawString("Built by", $chipFont, $mutedBrush, 378, 76)
    $builderBitmap = Get-BrandBitmap -ImagePath $BuilderLogoPath
    if ($builderBitmap) {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($builderBitmap, 344, 71, 22, 22)
        $builderBitmap.Dispose()
    }
    $graphics.DrawString("Boondock Labs", $builderFont, $builderBrush, 424, 72)

    $logoBitmap = Get-BrandBitmap -ImagePath $ProductLogoPath
    if ($logoBitmap) {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($logoBitmap, 625, 20, 74, 74)
        $logoBitmap.Dispose()
    }

    $brandFont.Dispose()
    $titleFont.Dispose()
    $chipFont.Dispose()
    $builderFont.Dispose()
    $brandBrush.Dispose()
    $titleBrush.Dispose()
    $mutedBrush.Dispose()
    $builderBrush.Dispose()
    $graphics.Dispose()

    Save-Bitmap -Bitmap $bitmap -Path $OutputPath
    $bitmap.Dispose()
}

function New-WizardGlyph {
    param(
        [string]$OutputPath,
        [string]$ProductLogoPath
    )

    $width = 55
    $height = 58
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    $bg = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $bg,
        (New-ThemeColor 56 56 56),
        (New-ThemeColor 24 24 24),
        65.0
    )
    $graphics.FillRectangle($gradient, $bg)
    $gradient.Dispose()

    Add-Grain -Graphics $graphics -Width $width -Height $height -Seed 314159 -Density 0.048 -MinAlpha 10 -MaxAlpha 30
    Add-Scanlines -Graphics $graphics -Width $width -Height $height

    $panelRect = [System.Drawing.RectangleF]::new(6, 5, 43, 48)
    $panelPath = New-RoundedRectanglePath -Rectangle $panelRect -Radius 10
    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(92, 12, 12, 12))
    $panelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(172, 92, 92, 92))
    $graphics.FillPath($panelBrush, $panelPath)
    $graphics.DrawPath($panelPen, $panelPath)
    $panelBrush.Dispose()
    $panelPen.Dispose()
    $panelPath.Dispose()

    $logoBitmap = Get-BrandBitmap -ImagePath $ProductLogoPath
    if ($logoBitmap) {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($logoBitmap, 10, 9, 35, 35)
        $logoBitmap.Dispose()
    }

    $graphics.Dispose()
    Save-Bitmap -Bitmap $bitmap -Path $OutputPath
    $bitmap.Dispose()
}

function New-WizardBackground {
    param(
        [int]$Width,
        [int]$Height,
        [string]$OutputPath
    )

    $bitmap = New-Object System.Drawing.Bitmap($Width, $Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    $bg = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $bg,
        (New-ThemeColor 70 70 70),
        (New-ThemeColor 22 22 22),
        42.0
    )
    $graphics.FillRectangle($gradient, $bg)
    $gradient.Dispose()

    Add-Glow -Graphics $graphics -CenterX ($Width * 0.22) -CenterY ($Height * 0.12) -RadiusX ($Width * 0.60) -RadiusY ($Height * 0.45) -Color (New-ThemeColor 188 188 188)
    Add-Glow -Graphics $graphics -CenterX ($Width * 0.80) -CenterY ($Height * 0.18) -RadiusX ($Width * 0.42) -RadiusY ($Height * 0.28) -Color (New-ThemeColor 92 92 92)

    $overlayBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(92, 14, 14, 14))
    $graphics.FillRectangle($overlayBrush, 0, 0, $Width, $Height)
    $overlayBrush.Dispose()

    $random = [System.Random]::new($Width + $Height)
    for ($i = 0; $i -lt 8; $i++) {
        $alpha = $random.Next(14, 28)
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb($alpha, 120, 120, 120))
        $inset = 24 + ($i * 22)
        $graphics.DrawArc($pen, -($Width * 0.15), -($Height * 0.10), ($Width * 0.80) + $inset, ($Height * 0.52) + $inset, 180, 96)
        $pen.Dispose()
    }

    Add-Scanlines -Graphics $graphics -Width $Width -Height $Height
    Add-Grain -Graphics $graphics -Width $Width -Height $Height -Seed ($Width * 31 + $Height) -Density 0.026 -MinAlpha 10 -MaxAlpha 34

    $framePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(72, 230, 230, 230))
    $graphics.DrawRectangle($framePen, 10, 10, $Width - 21, $Height - 21)
    $framePen.Dispose()

    $graphics.Dispose()
    Save-Bitmap -Bitmap $bitmap -Path $OutputPath
    $bitmap.Dispose()
}

$largePath = Join-Path $ProjectRoot "res\InstallerWizardLarge.bmp"
$smallPath = Join-Path $ProjectRoot "res\InstallerWizardSmall.bmp"
$background100Path = Join-Path $ProjectRoot "res\InstallerWizardBackground-100.png"
$background150Path = Join-Path $ProjectRoot "res\InstallerWizardBackground-150.png"
$productLogoPath = @(
    "res\biko_white.ico",
    "res\Biko.ico"
) | ForEach-Object { Join-Path $ProjectRoot $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
$builderLogoPath = @(
    "res\BoondockLabs.png",
    "res\boondock.ico"
) | ForEach-Object { Join-Path $ProjectRoot $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1

New-WizardBanner -OutputPath $largePath -ProductLogoPath $productLogoPath -BuilderLogoPath $builderLogoPath
New-WizardGlyph -OutputPath $smallPath -ProductLogoPath $productLogoPath
New-WizardBackground -Width 596 -Height 432 -OutputPath $background100Path
New-WizardBackground -Width 994 -Height 720 -OutputPath $background150Path

Write-Host "Wizard art updated:" -ForegroundColor Green
Write-Host "  $largePath"
Write-Host "  $smallPath"
Write-Host "  $background100Path"
Write-Host "  $background150Path"
