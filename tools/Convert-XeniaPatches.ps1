# Convert-XeniaPatches.ps1
# Prepares Xenia Canary patch files (https://github.com/xenia-canary/game-patches)
# for the Xbox 360 hard disk (FATX):
#   - ASCII-only names, max 42 characters including ".toml"
#   - the TU always right after the title ID, never cut off:
#       "545408A7 - Grand Theft Auto V (TU27).patch.toml" -> "545408A7 (TU27) Grand Theft Auto V.toml"
#   - every patch switched off (is_enabled = false); turn them on in Aurora
# The original files are not changed.
#
# Usage (PowerShell):
#   powershell -ExecutionPolicy Bypass -File Convert-XeniaPatches.ps1 -Src "C:\...\patches" -Dst "C:\...\Patches_xbox"
# Then copy the CONTENTS of -Dst to Hdd:\Patches\ on the console.

param(
    [Parameter(Mandatory = $true)][string]$Src,
    [Parameter(Mandatory = $true)][string]$Dst
)

$MaxLen = 42
$Ext    = ".toml"

function To-SafeAscii([string]$s) {
    $norm = $s.Normalize([Text.NormalizationForm]::FormD)
    $sb = New-Object Text.StringBuilder
    foreach ($ch in $norm.ToCharArray()) {
        if ([Globalization.CharUnicodeInfo]::GetUnicodeCategory($ch) -eq [Globalization.UnicodeCategory]::NonSpacingMark) { continue }
        $code = [int]$ch
        if ($code -lt 32 -or $code -gt 126) { continue }              # non-ASCII
        if ('<>:"/\|*?+,;='.Contains([string]$ch)) { continue }       # not allowed / risky on FATX
        if ($ch -eq '[') { [void]$sb.Append('('); continue }
        if ($ch -eq ']') { [void]$sb.Append(')'); continue }
        [void]$sb.Append($ch)
    }
    return ($sb.ToString() -replace '\s+', ' ').Trim(' ', '.', '-')
}

function Join-Name($head, $base, $tags, $suffix) {
    $rest = (($base, $tags) | Where-Object { $_ }) -join ' '
    if ($rest) { return "$head $rest$suffix$Ext" }
    return "$head$suffix$Ext"
}

# Shorten the game name first, the other tags only if there is no other way.
# The "<ID> (TUn)" head is never shortened.
function Build-Name($head, [string]$base, [string]$tags, $suffix) {
    $name = Join-Name $head $base $tags $suffix
    while ($name.Length -gt $MaxLen -and $base.Length -gt 0) {
        $base = $base.Substring(0, $base.Length - 1).TrimEnd(' ', '.', '-')
        $name = Join-Name $head $base $tags $suffix
    }
    while ($name.Length -gt $MaxLen -and $tags.Length -gt 0) {
        $tags = $tags.Substring(0, $tags.Length - 1)
        $name = Join-Name $head $base $tags $suffix
    }
    return $name
}

if (-not (Test-Path -LiteralPath $Src)) { Write-Error "Source folder not found: $Src"; exit 1 }
New-Item -ItemType Directory -Force -Path $Dst | Out-Null

$used = @{}; $copied = 0
Get-ChildItem -LiteralPath $Src -File -Filter *.toml | Sort-Object Name | ForEach-Object {
    $name = $_.Name -replace '\.patch\.toml$', '' -replace '\.toml$', ''
    if ($name -notmatch '^([0-9A-Fa-f]{8})\s*-?\s*(.*)$') { Write-Warning "Skipped (no title ID): $($_.Name)"; return }
    $id   = $Matches[1].ToUpper()
    $rest = To-SafeAscii $Matches[2]

    # TU number: from a (...) tag, even when merged ("...EditionTu1"), else from the name
    $tu = $null; $tags = ''
    foreach ($m in [regex]::Matches($rest, '\(([^)]*)\)')) {
        $t  = $m.Groups[1].Value
        $mm = [regex]::Match($t, '(?i)TU[ _-]?(\d+)')
        if ($mm.Success -and $tu -eq $null) {
            $tu = [int]$mm.Groups[1].Value
            $t  = ($t.Remove($mm.Index, $mm.Length)).Trim(' ', '_', '-', '.')
        }
        if ($t) { $tags += '(' + ($t -replace '\s+', '') + ')' }
    }
    $base = ([regex]::Replace($rest, '\([^)]*\)', ' ') -replace '\s+', ' ').Trim(' ', '.', '-')
    if ($tu -eq $null) {
        $mm = [regex]::Match($base, '(?i)\bTU[ _-]?(\d+)\b')
        if ($mm.Success) { $tu = [int]$mm.Groups[1].Value; $base = $base.Remove($mm.Index, $mm.Length).Trim(' ', '.', '-') }
    }

    $head = if ($tu -ne $null) { "$id (TU$tu)" } else { $id }
    $new  = Build-Name $head $base $tags ""
    $n = 2
    while ($used.ContainsKey($new.ToLower())) { $new = Build-Name $head $base $tags " $n"; $n++ }
    $used[$new.ToLower()] = $true

    $text = [IO.File]::ReadAllText($_.FullName)
    $text = [regex]::Replace($text, '(?m)^(\s*is_enabled\s*=\s*)true', '${1}false')
    [IO.File]::WriteAllText((Join-Path $Dst $new), $text)    # UTF-8 without BOM
    $copied++
}

Write-Host ""
Write-Host "$copied files written to $Dst"
Write-Host "Copy the contents of that folder to Hdd:\Patches\ on the console."
