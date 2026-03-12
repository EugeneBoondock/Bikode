[CmdletBinding()]
param(
    [string]$ReleaseTag = "v1.0.0",
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$ReleaseRepo = "EugeneBoondock/Bikode",
    [string]$MacRuntimeRepo = "Sikarugir-App/Engines",
    [string]$MacRuntimeTag = "v1.0",
    [string]$MacRuntimePattern = "WS11Wine10.0_3.tar.xz",
    [string]$WebsiteRoot = "C:\Users\USER\Projects\BikodeWebsite",
    [switch]$SkipBuild,
    [switch]$SkipUpload,
    [switch]$SkipWebsiteSync,
    [switch]$SyncWebsiteRepo
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

function Copy-IfDifferent {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    $resolvedSource = [System.IO.Path]::GetFullPath($SourcePath)
    $resolvedDestination = [System.IO.Path]::GetFullPath($DestinationPath)
    if ([string]::Equals($resolvedSource, $resolvedDestination, [System.StringComparison]::OrdinalIgnoreCase)) {
        return
    }

    Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$distDir = Join-Path $repoRoot "dist"
$runtimeCacheDir = Join-Path $distDir "mac-runtime-cache"
$version = $ReleaseTag.TrimStart('v')
$installerPath = Join-Path $distDir ("Bikode-Setup-{0}-x64.exe" -f $version)
$macAssetPath = Join-Path $distDir ("Bikode-mac-{0}.zip" -f $version)
$legacyMacAssetName = "Bikode-mac-wrapper-{0}.zip" -f $version
$stableInstallerName = "Bikode-Setup-stable-{0}.exe" -f $Platform
$stableInstallerPath = Join-Path $distDir $stableInstallerName
$stableMacAssetName = "Bikode-mac-stable.zip"
$stableMacAssetPath = Join-Path $distDir $stableMacAssetName
$runtimeArchivePath = Join-Path $runtimeCacheDir $MacRuntimePattern

New-Item -ItemType Directory -Path $runtimeCacheDir -Force | Out-Null

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-installer.ps1") -Configuration $Configuration -Platform $Platform
    Assert-LastExitCode "Windows installer build"
}

if (-not (Test-Path $installerPath)) {
    $latestInstaller = Get-ChildItem -Path $distDir -Filter "Bikode-Setup-*-x64.exe" |
        Where-Object { $_.Name -ne $stableInstallerName } |
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

Copy-IfDifferent -SourcePath $installerPath -DestinationPath $stableInstallerPath
Copy-IfDifferent -SourcePath $macAssetPath -DestinationPath $stableMacAssetPath

if (-not $SkipUpload) {
    $uploadPaths = @(
        $installerPath,
        $macAssetPath,
        $stableInstallerPath,
        $stableMacAssetPath
    )
    & gh release upload $ReleaseTag @uploadPaths --repo $ReleaseRepo --clobber
    Assert-LastExitCode "GitHub release upload"
}

if ($SyncWebsiteRepo -and -not $SkipWebsiteSync) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "sync-website-download.ps1") -InstallerPath $installerPath -MacAssetPath $macAssetPath -WebsiteRoot $WebsiteRoot
    Assert-LastExitCode "Website download sync"
} else {
    Write-Host "Website repo sync skipped. BikodeWebsite can point straight at GitHub release assets."
}

Write-Host "Release assets ready:"
Write-Host "  Windows: $installerPath"
Write-Host "  macOS:   $macAssetPath"
Write-Host "Stable release aliases:"
Write-Host "  Windows: $stableInstallerPath"
Write-Host "  macOS:   $stableMacAssetPath"
