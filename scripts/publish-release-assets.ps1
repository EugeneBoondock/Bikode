[CmdletBinding()]
param(
    [string]$ReleaseTag = "v1.0.0",
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$MacRuntimeRepo = "Sikarugir-App/Engines",
    [string]$MacRuntimeTag = "v1.0",
    [string]$MacRuntimePattern = "WS11Wine10.0_3.tar.xz",
    [string]$WebsiteRoot = "C:\Users\USER\Projects\BikodeWebsite",
    [switch]$SkipBuild,
    [switch]$SkipUpload,
    [switch]$SkipWebsiteSync
)

$ErrorActionPreference = "Stop"

function Assert-LastExitCode {
    param(
        [string]$Step
    )

    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE"
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$distDir = Join-Path $repoRoot "dist"
$runtimeCacheDir = Join-Path $distDir "mac-runtime-cache"
$version = $ReleaseTag.TrimStart('v')
$installerPath = Join-Path $distDir ("Bikode-Setup-{0}-x64.exe" -f $version)
$macAssetPath = Join-Path $distDir ("Bikode-mac-{0}.zip" -f $version)
$legacyMacAssetName = "Bikode-mac-wrapper-{0}.zip" -f $version
$runtimeArchivePath = Join-Path $runtimeCacheDir $MacRuntimePattern

New-Item -ItemType Directory -Path $runtimeCacheDir -Force | Out-Null

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-installer.ps1") -Configuration $Configuration -Platform $Platform
    Assert-LastExitCode "Windows installer build"
}

if (-not (Test-Path $installerPath)) {
    $latestInstaller = Get-ChildItem -Path $distDir -Filter "Bikode-Setup-*-x64.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($latestInstaller) {
        $installerPath = $latestInstaller.FullName
    }
}

if (-not (Test-Path $runtimeArchivePath)) {
    & gh release download $MacRuntimeTag --repo $MacRuntimeRepo --pattern $MacRuntimePattern -D $runtimeCacheDir
    Assert-LastExitCode "Mac runtime download"
}

& python (Join-Path $PSScriptRoot "build-mac-wrapper.py") --app-version $version --runtime-archive $runtimeArchivePath
Assert-LastExitCode "Mac wrapper build"

if (-not (Test-Path $installerPath)) {
    throw "Installer not found: $installerPath"
}
if (-not (Test-Path $macAssetPath)) {
    throw "Mac asset not found: $macAssetPath"
}

if (-not $SkipUpload) {
    & gh release delete-asset $ReleaseTag $legacyMacAssetName --repo EugeneBoondock/Biko -y 2>$null
    & gh release upload $ReleaseTag $installerPath $macAssetPath --repo EugeneBoondock/Biko --clobber
    Assert-LastExitCode "GitHub release upload"
}

if (-not $SkipWebsiteSync) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "sync-website-download.ps1") -InstallerPath $installerPath -MacAssetPath $macAssetPath -WebsiteRoot $WebsiteRoot
    Assert-LastExitCode "Website download sync"
}

Write-Host "Release assets ready:"
Write-Host "  Windows: $installerPath"
Write-Host "  macOS:   $macAssetPath"
