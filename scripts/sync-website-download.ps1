[CmdletBinding()]
param(
    [string]$InstallerPath,
    [string]$WebsiteRoot = "C:\Users\USER\Projects\BikodeWebsite"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
    $InstallerPath = Join-Path $PSScriptRoot "..\dist\Bikode-Setup-1.0-x64.exe"
}

$InstallerPath = (Resolve-Path $InstallerPath).Path
$WebsiteRoot = (Resolve-Path $WebsiteRoot).Path

$downloadDir = Join-Path $WebsiteRoot "public\downloads"
$stableInstaller = Join-Path $downloadDir "Bikode-Setup-stable-x64.exe"

if (-not (Test-Path $InstallerPath)) {
    throw "Installer not found: $InstallerPath"
}

New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
Copy-Item -Path $InstallerPath -Destination $stableInstaller -Force

Write-Host "Website download updated: $stableInstaller"
