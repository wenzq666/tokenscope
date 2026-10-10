[CmdletBinding()]
param([switch]$ShowUnused)
# Every literal passed to Tr / Tr8 / TrF / TrF8, every catalog note and every connection hint must
# have an English entry in src/i18n_en.inc. Missing keys fail the check; unused keys are reported.
# Keep this file ASCII: Windows PowerShell 5 reads BOM-less scripts in the ANSI code page.
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$utf8 = [Text.UTF8Encoding]::new($false)
$literal = '(?:L|u8)?"((?:[^"\\\r\n]|\\.)*)"'
$call = [regex]('\b(?:Tr|Tr8|TrF|TrF8)\(\s*((?:' + $literal + '\s*)+)')
$han = [regex]('[' + [char]0x4E00 + '-' + [char]0x9FFF + ']')
$keys = [Collections.Generic.Dictionary[string, string]]::new()

function Add-Key([string]$Key, [string]$Where) {
    if (-not $keys.ContainsKey($Key)) { $keys[$Key] = $Where }
}
function Read-Source([string]$Relative) {
    return [IO.File]::ReadAllText((Join-Path $repoRoot $Relative), $utf8)
}

foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -Filter '*.cpp') {
    $text = [IO.File]::ReadAllText($file.FullName, $utf8)
    $relative = $file.FullName.Substring($repoRoot.Length + 1)
    foreach ($match in $call.Matches($text)) {
        $parts = [regex]::Matches($match.Groups[1].Value, $literal) | ForEach-Object { $_.Groups[1].Value }
        Add-Key ($parts -join '') $relative
    }
}
# Static data translated where it is displayed: catalog notes (wide literals) and the connection
# profile table (every Chinese literal in connections.cpp).
foreach ($match in [regex]::Matches((Read-Source 'src/adapters/catalog.cpp'), 'L"((?:[^"\\\r\n]|\\.)*)"')) {
    if ($han.IsMatch($match.Groups[1].Value)) { Add-Key $match.Groups[1].Value 'src/adapters/catalog.cpp' }
}
foreach ($match in [regex]::Matches((Read-Source 'src/connections.cpp'), '"((?:[^"\\\r\n]|\\.)*)"')) {
    if ($han.IsMatch($match.Groups[1].Value)) { Add-Key $match.Groups[1].Value 'src/connections.cpp' }
}
# Stored price-source fragments translated through a loop variable in DisplayPriceSource.
$view = Read-Source 'src/view.cpp'
$parts = [regex]::Match($view, 'for \(const wchar_t\* part : \{([^}]*)\}\)')
if (-not $parts.Success) { throw 'DisplayPriceSource fragment list not found in src/view.cpp.' }
foreach ($match in [regex]::Matches($parts.Groups[1].Value, $literal)) { Add-Key $match.Groups[1].Value 'src/view.cpp' }

$table = [Collections.Generic.HashSet[string]]::new()
foreach ($line in [IO.File]::ReadAllLines((Join-Path $repoRoot 'src/i18n_en.inc'), $utf8)) {
    $entry = [regex]::Match($line, '^\{L"((?:[^"\\]|\\.)*)", L"')
    if ($entry.Success -and -not $table.Add($entry.Groups[1].Value)) {
        Write-Warning "Duplicate translation key: $($entry.Groups[1].Value)"
    }
}

$missing = @($keys.Keys | Where-Object { -not $table.Contains($_) })
foreach ($key in $missing) { Write-Host "Missing English text ($($keys[$key])): $key" }
if ($ShowUnused) {
    foreach ($key in $table) { if (-not $keys.ContainsKey($key)) { Write-Host "Unused translation: $key" } }
}
if ($missing.Count -gt 0) { throw "$($missing.Count) interface strings have no English translation in src/i18n_en.inc." }
Write-Host "Translations complete: $($keys.Count) keys used, $($table.Count) entries."
