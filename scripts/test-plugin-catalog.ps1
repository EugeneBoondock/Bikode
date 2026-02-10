$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

Write-Host "Testing Notepad plugin catalog endpoint..."
$nppUrl = "https://raw.githubusercontent.com/notepad-plus-plus/nppPluginList/master/src/pl.x64.json"
$npp = Invoke-RestMethod -Uri $nppUrl

$items = $npp."npp-plugins"
if (-not $items -or $items.Count -le 0) {
    Write-Error "No plugins returned from Notepad plugin list."
    exit 1
}

Write-Host ("Notepad plugin count: " + $items.Count)

$popularNames = @(
    "NppExec",
    "Compare",
    "PythonScript",
    "JSON Viewer",
    "Explorer",
    "XML Tools",
    "HEX-Editor",
    "JSTool",
    "MarkdownViewer++",
    "Npp Converter"
)

Write-Host ""
Write-Host "Popular plugin samples found:"
$found = 0
foreach ($name in $popularNames) {
    $p = $items | Where-Object { $_."display-name" -eq $name } | Select-Object -First 1
    if ($p) {
        $found++
        Write-Host ("- " + $p."display-name" + " | " + $p.repository)
    }
}

Write-Host ""
Write-Host "Top 12 catalog entries:"
$items |
    Select-Object -First 12 -Property "display-name", "repository", "author" |
    Format-Table -AutoSize |
    Out-String -Width 240 |
    Write-Host

if ($found -lt 3) {
    Write-Error "Catalog is reachable but expected popular plugins were not found."
    exit 1
}

Write-Host ""
Write-Host "Testing Open VSX popular catalog endpoint..."
$ovsxUrl = "https://open-vsx.org/api/-/search?size=20&offset=0&sortBy=downloadCount&sortOrder=desc"
$ovsx = Invoke-RestMethod -Uri $ovsxUrl
$extensions = $ovsx.extensions
if (-not $extensions -or $extensions.Count -le 0) {
    Write-Error "No extensions returned from Open VSX search endpoint."
    exit 1
}

Write-Host ("Open VSX extension count: " + $extensions.Count)
Write-Host ""
Write-Host "Top 10 Open VSX entries:"
$extensions |
    Select-Object -First 10 -Property `
        @{n="id";e={ $_.namespace + "." + $_.name }},
        displayName,
        version,
        downloadCount |
    Format-Table -AutoSize |
    Out-String -Width 240 |
    Write-Host

Write-Host "Plugin catalog test passed."
exit 0
