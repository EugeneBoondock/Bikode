[CmdletBinding()]
param(
    [string]$InstallerPath,
    [string]$MacAssetPath,
    [string]$WebsiteRoot = "C:\Users\USER\Projects\BikodeWebsite"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
    $InstallerPath = Join-Path $PSScriptRoot "..\dist\Bikode-Setup-1.0-x64.exe"
}
if ([string]::IsNullOrWhiteSpace($MacAssetPath)) {
    $MacAssetPath = Join-Path $PSScriptRoot "..\dist\Bikode-mac-1.0.0.zip"
}

$InstallerPath = (Resolve-Path $InstallerPath).Path
$MacAssetPath = (Resolve-Path $MacAssetPath).Path
$WebsiteRoot = (Resolve-Path $WebsiteRoot).Path

$downloadDir = Join-Path $WebsiteRoot "public\downloads"
$stableInstaller = Join-Path $downloadDir "Bikode-Setup-stable-x64.exe"
$stableMacAsset = Join-Path $downloadDir "Bikode-mac-stable.zip"

if (-not (Test-Path $InstallerPath)) {
    throw "Installer not found: $InstallerPath"
}
if (-not (Test-Path $MacAssetPath)) {
    throw "Mac asset not found: $MacAssetPath"
}

New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
Copy-Item -Path $InstallerPath -Destination $stableInstaller -Force
Copy-Item -Path $MacAssetPath -Destination $stableMacAsset -Force

Write-Host "Website download updated: $stableInstaller"
Write-Host "Website download updated: $stableMacAsset"
