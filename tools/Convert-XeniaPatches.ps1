# Convert-XeniaPatches.ps1 (XPatch360 v1.2)
# Prepares Xenia Canary patch files (https://github.com/xenia-canary/game-patches)
# for the Xbox 360 hard disk (FATX):
#   - ASCII-only names, max 42 characters including ".toml"
#   - the TU always right after the title ID, never cut off:
#       "545408A7 - Grand Theft Auto V (TU27).patch.toml" -> "545408A7 (TU27) Grand Theft Auto V.toml"
#   - every patch switched off (is_enabled = false); turn them on in Aurora
#   - console fixes (converter-fixes folder next to this script, files named <ID>_<game>_<tu>.toml,
#     e.g. 5454082B_rdr_tu9.toml; the name is only for people, the title_id / hash inside decide):
#       title_id + hash + remove: for the Xenia file with that title ID and hash, the patches listed
#                                 in "remove" are taken out and the [[patch]] blocks of the fix are added
#                                 (patches that crash or do nothing on a real console, console versions)
#       title_id + skip = true  : no file of that title ID is converted
# The original files are not changed.
#
# Usage (PowerShell):
#   powershell -ExecutionPolicy Bypass -File Convert-XeniaPatches.ps1 -Src "C:\...\patches" -Dst "C:\...\Patches_xbox"
#   optional: -Fixes "C:\...\converter-fixes"   (default: the converter-fixes folder next to this script)
# Then copy the CONTENTS of -Dst to Hdd:\Patches\ on the console.

param(
    [Parameter(Mandatory = $true)][string]$Src,
    [Parameter(Mandatory = $true)][string]$Dst,
    [string]$Fixes = ""
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


# ---- console fixes ---------------------------------------------------------
# A TOML file is split at the top-level "[[patch]]" lines (the patch.be32 etc. sub-tables
# are always indented in Xenia files and in the fixes): part 0 = header, then one part per patch.
function Split-Patches([string]$text) {
    $starts = [regex]::Matches($text, '(?m)^\[\[patch\]\][ \t]*\r?$')
    $parts = New-Object System.Collections.ArrayList
    if ($starts.Count -eq 0) { [void]$parts.Add($text); return ,$parts }
    [void]$parts.Add($text.Substring(0, $starts[0].Index))
    for ($i = 0; $i -lt $starts.Count; $i++) {
        $end = if ($i + 1 -lt $starts.Count) { $starts[$i + 1].Index } else { $text.Length }
        [void]$parts.Add($text.Substring($starts[$i].Index, $end - $starts[$i].Index))
    }
    return ,$parts
}

function Get-PatchName([string]$block) {
    $m = [regex]::Match($block, '(?m)^\s*name\s*=\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# all 16-digit hex values of the "hash = ..." line(s) of a header (a string or a list)
function Get-Hashes([string]$header) {
    $list = @()
    $m = [regex]::Match($header, '(?ms)^\s*hash\s*=\s*(\[[^\]]*\]|"[^"]*")')
    if ($m.Success) {
        foreach ($h in [regex]::Matches($m.Groups[1].Value, '[0-9A-Fa-f]{16}')) { $list += $h.Value.ToUpper() }
    }
    return $list
}

if (-not $Fixes) { $Fixes = Join-Path $PSScriptRoot 'converter-fixes' }
$fixByKey = @{}; $skipId = @{}
if (Test-Path -LiteralPath $Fixes) {
    Get-ChildItem -LiteralPath $Fixes -File -Filter *.toml | ForEach-Object {
        $t     = [IO.File]::ReadAllText($_.FullName)
        $parts = Split-Patches $t
        $hdr   = $parts[0]
        $idm   = [regex]::Match($hdr, '(?m)^\s*title_id\s*=\s*"([0-9A-Fa-f]{8})"')
        if (-not $idm.Success) { Write-Warning "Fix without title_id, ignored: $($_.Name)"; return }
        $fid = $idm.Groups[1].Value.ToUpper()
        if ($hdr -match '(?m)^\s*skip\s*=\s*true') { $skipId[$fid] = $_.Name; return }
        $remove = @()
        $rm = [regex]::Match($hdr, '(?ms)^\s*remove\s*=\s*\[([^\]]*)\]')
        if ($rm.Success) { foreach ($n in [regex]::Matches($rm.Groups[1].Value, '"([^"]*)"')) { $remove += $n.Groups[1].Value } }
        $add = ($parts | Select-Object -Skip 1) -join ''
        foreach ($h in (Get-Hashes $hdr)) {
            $fixByKey["$fid $h"] = @{ Name = $_.Name; Remove = $remove; Add = $add; Count = $parts.Count - 1 }
        }
    }
    Write-Host "Console fixes: $($fixByKey.Count) game version(s), $($skipId.Count) skipped game(s) ($Fixes)"
} else {
    Write-Warning "converter-fixes folder not found ($Fixes) - converting without console fixes"
}

if (-not (Test-Path -LiteralPath $Src)) { Write-Error "Source folder not found: $Src"; exit 1 }
New-Item -ItemType Directory -Force -Path $Dst | Out-Null

$used = @{}; $copied = 0; $skipped = 0; $fixed = 0
Get-ChildItem -LiteralPath $Src -File -Filter *.toml | Sort-Object Name | ForEach-Object {
    $name = $_.Name -replace '\.patch\.toml$', '' -replace '\.toml$', ''
    if ($name -notmatch '^([0-9A-Fa-f]{8})\s*-?\s*(.*)$') { Write-Warning "Skipped (no title ID): $($_.Name)"; return }
    $id   = $Matches[1].ToUpper()
    $rest = To-SafeAscii $Matches[2]
    if ($skipId.ContainsKey($id)) { Write-Host "Skipped ($($skipId[$id])): $($_.Name)"; $skipped++; return }

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

    # console fix for this game version (title ID + hash)
    $parts = Split-Patches $text
    foreach ($h in (Get-Hashes $parts[0])) {
        $fix = $fixByKey["$id $h"]
        if (-not $fix) { continue }
        $keep = @($parts[0]); $removed = 0
        foreach ($p in ($parts | Select-Object -Skip 1)) {
            if ($fix.Remove -contains (Get-PatchName $p)) { $removed++ } else { $keep += $p }
        }
        $body = ($keep -join '').TrimEnd() + "`n"
        if ($fix.Add) { $body += "`n" + $fix.Add.TrimEnd() + "`n" }
        $text = $body
        Write-Host "Console fix $($fix.Name): $removed removed, $($fix.Count) added -> $new"
        $fixed++
        break
    }

    $text = [regex]::Replace($text, '(?m)^(\s*is_enabled\s*=\s*)true', '${1}false')
    [IO.File]::WriteAllText((Join-Path $Dst $new), $text)    # UTF-8 without BOM
    $copied++
}

Write-Host ""
Write-Host "$copied files written to $Dst ($fixed with console fixes, $skipped skipped)"
Write-Host "Copy the contents of that folder to Hdd:\Patches\ on the console."
