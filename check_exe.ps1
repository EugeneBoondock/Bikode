$bytes = [IO.File]::ReadAllBytes('c:\Users\USER\Projects\Biko\bin\x64\Debug\Biko.exe')
$text = [Text.Encoding]::Unicode.GetString($bytes)
$searches = @('BikoTermView', 'BikoTermPanel', '[v5]', 'Terminal_Init', 'Terminal_Toggle', 'VIEW HAS FOCUS')
foreach ($s in $searches) {
    if ($text.Contains($s)) {
        Write-Host "FOUND: $s"
    } else {
        Write-Host "NOT FOUND: $s"
    }
}
# Also check ASCII encoding
$atext = [Text.Encoding]::ASCII.GetString($bytes)
$asearches = @('BikoTermView', 'Terminal_Init', 'ViewProc', 'v5-CLEAN')
foreach ($s in $asearches) {
    if ($atext.Contains($s)) {
        Write-Host "FOUND (ASCII): $s"
    } else {
        Write-Host "NOT FOUND (ASCII): $s"
    }
}
