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
        [string]$BuilderLogoPath,
        [string]$ArtworkPath
    )

    if ($ArtworkPath -and (Test-Path $ArtworkPath)) {
        $width = 164
        $height = 314
        $bitmap = New-Object System.Drawing.Bitmap($width, $height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

        $bg = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
        $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            $bg,
            (New-ThemeColor 58 58 58),
            (New-ThemeColor 20 20 20),
            90.0
        )
        $graphics.FillRectangle($gradient, $bg)
        $gradient.Dispose()

        Add-Glow -Graphics $graphics -CenterX 44 -CenterY 36 -RadiusX 96 -RadiusY 156 -Color (New-ThemeColor 210 210 210)
        Add-Glow -Graphics $graphics -CenterX 122 -CenterY 220 -RadiusX 88 -RadiusY 132 -Color (New-ThemeColor 75 139 245)
        Add-Scanlines -Graphics $graphics -Width $width -Height $height
        Add-Grain -Graphics $graphics -Width $width -Height $height -Seed 314159 -Density 0.032 -MinAlpha 10 -MaxAlpha 28

        $art = Get-BrandBitmap -ImagePath $ArtworkPath
        if ($art) {
            try {
                $drawX = [int](($width - $art.Width) / 2)
                $drawY = [int](($height - $art.Height) / 2)
                $graphics.DrawImage($art, $drawX, $drawY, $art.Width, $art.Height)
            } finally {
                $art.Dispose()
            }
        }

        $lineBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 75 139 245 210))
        $graphics.FillRectangle($lineBrush, 10, 18, 5, ($height - 36))
        $lineBrush.Dispose()

        $framePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(78, 228, 232, 240))
        $graphics.DrawRectangle($framePen, 6, 6, $width - 13, $height - 13)
        $framePen.Dispose()

        $graphics.Dispose()
        Save-Bitmap -Bitmap $bitmap -Path $OutputPath
        $bitmap.Dispose()
        return
    }

    $width = 164
    $height = 314
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    $bg = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $topColor = New-ThemeColor 58 62 72
    $bottomColor = New-ThemeColor 18 18 18
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush($bg, $topColor, $bottomColor, 90.0)
    $graphics.FillRectangle($gradient, $bg)
    $gradient.Dispose()

    Add-Glow -Graphics $graphics -CenterX 42 -CenterY 40 -RadiusX 90 -RadiusY 150 -Color (New-ThemeColor 218 218 218)
    Add-Glow -Graphics $graphics -CenterX 126 -CenterY 216 -RadiusX 88 -RadiusY 144 -Color (New-ThemeColor 96 96 96)

    $overlayBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(78, 14, 14, 14))
    $graphics.FillRectangle($overlayBrush, 0, 0, $width, $height)
    $overlayBrush.Dispose()

    Add-Scanlines -Graphics $graphics -Width $width -Height $height
    Add-Grain -Graphics $graphics -Width $width -Height $height -Seed 271828 -Density 0.034 -MinAlpha 12 -MaxAlpha 34

    $panelRect = [System.Drawing.RectangleF]::new(16, 14, 132, 286)
    $panelPath = New-RoundedRectanglePath -Rectangle $panelRect -Radius 14
    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(124, 18, 18, 18))
    $panelBorder = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(192, 92, 92, 92))
    $graphics.FillPath($panelBrush, $panelPath)
    $graphics.DrawPath($panelBorder, $panelPath)
    $panelBrush.Dispose()
    $panelBorder.Dispose()
    $panelPath.Dispose()

    $brandFont = New-Object System.Drawing.Font("Segoe UI Black", 20, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $titleFont = New-Object System.Drawing.Font("Consolas", 8.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $chipFont = New-Object System.Drawing.Font("Consolas", 7.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $builderFont = New-Object System.Drawing.Font("Segoe UI Semibold", 8.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

    $brandBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 236 236 236))
    $titleBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 188 188 188))
    $mutedBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 136 136 136))
    $builderBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 226 226 226))
    $graphics.DrawString("BIKODE", $brandFont, $brandBrush, 28, 30)
    $graphics.DrawString("AI-FIRST IDE", $chipFont, $mutedBrush, 30, 56)
    $graphics.DrawString("Grey grain.`nReal logo.`nComic energy.", $titleFont, $titleBrush, 30, 206)

    Draw-Chip -Graphics $graphics -X 28 -Y 14 -Width 68 -Height 17 `
        -FillColor (New-ThemeColor 26 26 26 210) `
        -BorderColor (New-ThemeColor 112 112 112) `
        -TextColor (New-ThemeColor 216 216 216) `
        -Text "SETUP" `
        -Font $chipFont

    $ruleBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 210 210 210))
    $graphics.FillRectangle($ruleBrush, 28, 68, 90, 1)
    $ruleBrush.Dispose()

    $logoPanel = [System.Drawing.RectangleF]::new(28, 84, 108, 104)
    $logoPanelPath = New-RoundedRectanglePath -Rectangle $logoPanel -Radius 12
    $logoPanelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(88, 8, 8, 8))
    $logoPanelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(156, 86, 86, 86))
    $graphics.FillPath($logoPanelBrush, $logoPanelPath)
    $graphics.DrawPath($logoPanelPen, $logoPanelPath)
    $logoPanelBrush.Dispose()
    $logoPanelPen.Dispose()
    $logoPanelPath.Dispose()

    $builderPanel = [System.Drawing.RectangleF]::new(28, 250, 108, 34)
    $builderPanelPath = New-RoundedRectanglePath -Rectangle $builderPanel -Radius 10
    $builderPanelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(88, 10, 10, 10))
    $builderPanelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(132, 118, 118, 118))
    $graphics.FillPath($builderPanelBrush, $builderPanelPath)
    $graphics.DrawPath($builderPanelPen, $builderPanelPath)
    $builderPanelBrush.Dispose()
    $builderPanelPen.Dispose()
    $builderPanelPath.Dispose()

    $builderIconPanel = [System.Drawing.RectangleF]::new(32, 253, 24, 24)
    $builderIconPanelPath = New-RoundedRectanglePath -Rectangle $builderIconPanel -Radius 8
    $builderIconBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(220, 54, 54, 54))
    $builderIconPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(160, 75, 139, 245))
    $graphics.FillPath($builderIconBrush, $builderIconPanelPath)
    $graphics.DrawPath($builderIconPen, $builderIconPanelPath)
    $builderIconBrush.Dispose()
    $builderIconPen.Dispose()
    $builderIconPanelPath.Dispose()

    $graphics.DrawString("Built by", $chipFont, $mutedBrush, 60, 255)
    $builderBitmap = Get-BrandBitmap -ImagePath $BuilderLogoPath
    if ($builderBitmap) {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($builderBitmap, 35, 256, 18, 18)
        $builderBitmap.Dispose()
    }
    $graphics.DrawString("Boondock Labs", $builderFont, $builderBrush, 60, 267)

    $logoBitmap = Get-BrandBitmap -ImagePath $ProductLogoPath
    if ($logoBitmap) {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($logoBitmap, 47, 98, 70, 70)
        $logoBitmap.Dispose()
    }

    $accentBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 75 139 245))
    $graphics.FillRectangle($accentBrush, 16, 18, 6, 278)
    $accentBrush.Dispose()

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
    $height = 55
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

    $panelRect = [System.Drawing.RectangleF]::new(5, 5, 45, 45)
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
        $graphics.DrawImage($logoBitmap, 7, 18, 12, 12)
        $logoBitmap.Dispose()
    }

    $wordmarkFont = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $format = [System.Drawing.StringFormat]::GenericTypographic
    $format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoClip

    $lightBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 236 238 242))
    $blueBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 75 139 245))
    $shadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(110, 0, 0, 0))

    $segments = @(
        @{ Text = "BI"; Brush = $lightBrush },
        @{ Text = "KO"; Brush = $blueBrush },
        @{ Text = "DE"; Brush = $lightBrush }
    )

    $x = 21.0
    foreach ($segment in $segments) {
        $graphics.DrawString($segment.Text, $wordmarkFont, $shadowBrush, $x + 1, 19, $format)
        $graphics.DrawString($segment.Text, $wordmarkFont, $segment.Brush, $x, 18, $format)
        $segmentSize = $graphics.MeasureString($segment.Text, $wordmarkFont, 100, $format)
        $x += [Math]::Floor($segmentSize.Width) - 1
    }

    $accentBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 75 139 245 216))
    $graphics.FillRectangle($accentBrush, 9, 41, 37, 4)

    $accentBrush.Dispose()
    $shadowBrush.Dispose()
    $blueBrush.Dispose()
    $lightBrush.Dispose()
    $wordmarkFont.Dispose()
    $format.Dispose()

    $graphics.Dispose()
    Save-Bitmap -Bitmap $bitmap -Path $OutputPath
    $bitmap.Dispose()
}

function New-TasksPreview {
    param(
        [string]$OutputPath,
        [string]$ArtworkPath
    )

    $width = 236
    $height = 236
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    $bg = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $bg,
        (New-ThemeColor 48 48 48),
        (New-ThemeColor 20 20 20),
        45.0
    )
    $graphics.FillRectangle($gradient, $bg)
    $gradient.Dispose()

    Add-Glow -Graphics $graphics -CenterX ($width * 0.34) -CenterY ($height * 0.18) -RadiusX ($width * 0.78) -RadiusY ($height * 0.54) -Color (New-ThemeColor 75 139 245)
    Add-Scanlines -Graphics $graphics -Width $width -Height $height
    Add-Grain -Graphics $graphics -Width $width -Height $height -Seed 271828 -Density 0.028 -MinAlpha 10 -MaxAlpha 30

    $panelRect = [System.Drawing.RectangleF]::new(10, 10, $width - 20, $height - 20)
    $panelPath = New-RoundedRectanglePath -Rectangle $panelRect -Radius 12
    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(112, 12, 12, 12))
    $panelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(146, 75, 139, 245))
    $graphics.FillPath($panelBrush, $panelPath)
    $graphics.DrawPath($panelPen, $panelPath)
    $panelBrush.Dispose()
    $panelPen.Dispose()
    $panelPath.Dispose()

    if ($ArtworkPath -and (Test-Path $ArtworkPath)) {
        $art = Get-BrandBitmap -ImagePath $ArtworkPath
        if ($art) {
            try {
                $padding = 18
                $scale = [Math]::Min(($width - ($padding * 2)) / $art.Width, ($height - ($padding * 2)) / $art.Height)
                $drawW = [int]($art.Width * $scale)
                $drawH = [int]($art.Height * $scale)
                $drawX = [int](($width - $drawW) / 2)
                $drawY = [int](($height - $drawH) / 2)
                $graphics.DrawImage($art, $drawX, $drawY, $drawW, $drawH)
            } finally {
                $art.Dispose()
            }
        }
    }

    $accentBrush = New-Object System.Drawing.SolidBrush((New-ThemeColor 75 139 245 216))
    $graphics.FillRectangle($accentBrush, 10, $height - 18, $width - 20, 8)
    $accentBrush.Dispose()

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
    Add-Glow -Graphics $graphics -CenterX ($Width * 0.76) -CenterY ($Height * 0.18) -RadiusX ($Width * 0.44) -RadiusY ($Height * 0.30) -Color (New-ThemeColor 75 139 245)

    $overlayBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(92, 14, 14, 14))
    $graphics.FillRectangle($overlayBrush, 0, 0, $Width, $Height)
    $overlayBrush.Dispose()

    $random = [System.Random]::new($Width + $Height)
    for ($i = 0; $i -lt 8; $i++) {
        $alpha = $random.Next(14, 28)
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb($alpha, 86, 102, 140))
        $inset = 24 + ($i * 22)
        $graphics.DrawArc($pen, -($Width * 0.15), -($Height * 0.10), ($Width * 0.80) + $inset, ($Height * 0.52) + $inset, 180, 96)
        $pen.Dispose()
    }

    Add-Scanlines -Graphics $graphics -Width $Width -Height $Height
    Add-Grain -Graphics $graphics -Width $Width -Height $Height -Seed ($Width * 31 + $Height) -Density 0.026 -MinAlpha 10 -MaxAlpha 34

    $framePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(88, 130, 164, 230))
    $graphics.DrawRectangle($framePen, 10, 10, $Width - 21, $Height - 21)
    $framePen.Dispose()

    $graphics.Dispose()
    Save-Bitmap -Bitmap $bitmap -Path $OutputPath
    $bitmap.Dispose()
}

$largePath = Join-Path $ProjectRoot "res\InstallerWizardLarge.bmp"
$smallPath = Join-Path $ProjectRoot "res\InstallerWizardSmall.bmp"
$tasksPreviewPath = Join-Path $ProjectRoot "res\InstallerTasksPreview.bmp"
$background100Path = Join-Path $ProjectRoot "res\InstallerWizardBackground-100.png"
$background150Path = Join-Path $ProjectRoot "res\InstallerWizardBackground-150.png"
$wizardLargeArtworkPath = Join-Path $ProjectRoot "src\images\setup1.bmp"
$tasksPreviewArtworkPath = Join-Path $ProjectRoot "src\images\setup_image.png"
$productLogoPath = @(
    "res\biko_white.ico",
    "res\Biko.ico"
) | ForEach-Object { Join-Path $ProjectRoot $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
$builderLogoPath = @(
    "res\BoondockLabs.png",
    "res\boondock.ico"
) | ForEach-Object { Join-Path $ProjectRoot $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1

New-WizardBanner -OutputPath $largePath -ProductLogoPath $productLogoPath -BuilderLogoPath $builderLogoPath -ArtworkPath $wizardLargeArtworkPath
New-WizardGlyph -OutputPath $smallPath -ProductLogoPath $productLogoPath
New-TasksPreview -OutputPath $tasksPreviewPath -ArtworkPath $tasksPreviewArtworkPath
New-WizardBackground -Width 596 -Height 432 -OutputPath $background100Path
New-WizardBackground -Width 994 -Height 720 -OutputPath $background150Path

Write-Host "Wizard art updated:" -ForegroundColor Green
Write-Host "  $largePath"
Write-Host "  $smallPath"
Write-Host "  $tasksPreviewPath"
Write-Host "  $background100Path"
Write-Host "  $background150Path"
