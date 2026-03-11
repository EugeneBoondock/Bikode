[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",
    [string]$OutputBaseName,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Get-MSBuildPath {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "MSBuild.exe was not found. Install Visual Studio Build Tools 2022."
}

function Get-ISCCPath {
    $command = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "ISCC.exe was not found. Install Inno Setup 6."
}

function Remove-StaleInstallers {
    param(
        [string]$Directory,
        [string]$KeepFileName
    )

    if (-not (Test-Path $Directory)) {
        return
    }

    Get-ChildItem -Path $Directory -Filter "Bikode-Setup-*.exe" -File |
        Where-Object { $_.Name -ne $KeepFileName } |
        ForEach-Object {
            try {
                Remove-Item -Path $_.FullName -Force
            } catch {
                Write-Warning ("Could not remove old installer: {0}" -f $_.FullName)
            }
        }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectFile = Join-Path $repoRoot "Notepad2e.vcxproj"
$outputDir = Join-Path $repoRoot ("bin\{0}\{1}" -f $Platform, $Configuration)
$exePath = Join-Path $outputDir "Bikode.exe"
$iniPath = Join-Path $outputDir "Bikode.ini"
$defaultIniPath = Join-Path $repoRoot "bin\Bikode.ini"
$installerScript = Join-Path $repoRoot "installer\Bikode.iss"
$artScript = Join-Path $repoRoot "scripts\generate-installer-art.ps1"
$distDir = Join-Path $repoRoot "dist"
$buildArch = if ($Platform -eq "x64") { "x64" } else { "x86" }

if (-not (Test-Path $projectFile)) {
    throw "Project file not found: $projectFile"
}

if (-not (Test-Path $installerScript)) {
    throw "Installer script not found: $installerScript"
}

if (-not (Test-Path $artScript)) {
    throw "Installer art script not found: $artScript"
}

if (-not $SkipBuild) {
    $msbuild = Get-MSBuildPath
    $msbuildArgs = @(
        $projectFile,
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/p:PlatformToolset=v143",
        "/t:Build",
        "/m",
        "/v:minimal"
    )

    & $msbuild @msbuildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path $exePath)) {
    throw "Expected build output not found: $exePath"
}

if ((-not (Test-Path $iniPath)) -and (Test-Path $defaultIniPath)) {
    Copy-Item -Path $defaultIniPath -Destination $iniPath -Force
}

$appVersion = (Get-Item $exePath).VersionInfo.FileVersion
if ([string]::IsNullOrWhiteSpace($appVersion)) {
    $appVersion = "0.0.0"
}
$appVersion = $appVersion.Trim()
$defaultOutputBaseName = "Bikode-Setup-{0}-{1}" -f $appVersion, $buildArch
$resolvedOutputBaseName = if ([string]::IsNullOrWhiteSpace($OutputBaseName)) {
    $defaultOutputBaseName
} else {
    $OutputBaseName.Trim()
}
$targetInstaller = Join-Path $distDir ($resolvedOutputBaseName + ".exe")

New-Item -ItemType Directory -Path $distDir -Force | Out-Null

if (Test-Path $targetInstaller) {
    try {
        Remove-Item -Path $targetInstaller -Force
    } catch {
        throw "Close the existing installer before rebuilding: $targetInstaller"
    }
}

& $artScript -ProjectRoot $repoRoot

$iscc = Get-ISCCPath
$isccArgs = @(
    "/DAppVersion=$appVersion",
    "/DBuildArch=$buildArch",
    "/DSourceDir=$outputDir",
    "/DProjectRoot=$repoRoot",
    "/DOutputDir=$distDir"
    "/DOutputBaseName=$resolvedOutputBaseName"
)

$isccArgs += $installerScript

& $iscc @isccArgs
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup build failed with exit code $LASTEXITCODE."
}

Remove-StaleInstallers -Directory $distDir -KeepFileName ([System.IO.Path]::GetFileName($targetInstaller))

Write-Host "Installer created: $targetInstaller"
