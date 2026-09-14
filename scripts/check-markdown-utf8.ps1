<#
.SYNOPSIS
    Validate and repair UTF-8 encoding in project markdown files.

.DESCRIPTION
    Scans markdown / text files for known UTF-8 corruption patterns that
    result from editing in Windows console codepage (cp1252) mode or
    from a buggy cp1252<->UTF-8 round-trip tool.

    Corruption catalog (all bytes shown in hex):

      cp1252 orphan single bytes (preceded by ASCII, not a UTF-8 lead):
        97                  -> E2 80 94   em-dash          (U+2014)
        A7                  -> C2 A7      section sign     (U+00A7)
        D7                  -> C3 97      multiplication   (U+00D7)

      2-byte or 3-byte sequences whose last byte was corrupted to 3F (?):
        EF BF 3F            -> E2 80 94   em-dash          (U+2014)
        E2 80 3F            -> E2 80 94   em-dash          (U+2014)
        E2 86 3F            -> E2 86 92   right arrow      (U+2192)
        E2 9C 3F            -> E2 9C 85   check mark       (U+2705)
        C2 9D               -> C2 A7      section sign     (U+00A7)

      8-byte CJK fallback (cp1252 fallback for U+1F536 large orange diamond):
        E9 A6 83 E6 95 B9   -> F0 9F 94 B6   orange diamond (U+1F536)

    Plus a generic safety check: any byte >= 0x80 that is NOT part of a
    valid UTF-8 sequence produces U+FFFD on decode and is treated as a
    hard error (the script cannot guess what was intended).

    The script is idempotent: running on already-clean files does nothing
    and reports zero changes.

.PARAMETER Path
    File or directory to scan. Defaults to repo root (D:\MyOpen\NimRTC).

.PARAMETER Fix
    Apply repairs in place. Without -Fix, the script only reports.

.PARAMETER Quiet
    Suppress per-file "clean" output; only print files with issues.

.PARAMETER Pattern
    Glob of files to include (relative to Path). Defaults to '*.md', '*.txt'.

.EXAMPLE
    pwsh scripts/check-markdown-utf8.ps1
    pwsh scripts/check-markdown-utf8.ps1 -Fix
    pwsh scripts/check-markdown-utf8.ps1 CHANGELOG.md -Fix
    pwsh scripts/check-markdown-utf8.ps1 docs -Fix -Quiet

.NOTES
    Exit code: 0 if all scanned files are clean (or successfully fixed),
    1 if any unrecoverable encoding errors remain.
#>

[CmdletBinding()]
param(
    [string]$Path = (Split-Path -Parent $PSScriptRoot),
    [switch]$Fix,
    [switch]$Quiet,
    [string[]]$Pattern = @('*.md', '*.txt')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- Patterns --------------------------------------------------------------
# Each entry: <search-bytes> -> <replace-bytes>
# Search is byte-exact; replacement uses literal UTF-8 byte sequences.
$replacements = @(
    @{ Name = 'em-dash (EF BF 3F)';         Search = @(0xEF,0xBF,0x3F);                Replace = @(0xE2,0x80,0x94) },
    @{ Name = 'em-dash (E2 80 3F)';        Search = @(0xE2,0x80,0x3F);                Replace = @(0xE2,0x80,0x94) },
    @{ Name = 'arrow (E2 86 3F)';           Search = @(0xE2,0x86,0x3F);                Replace = @(0xE2,0x86,0x92) },
    @{ Name = 'check-mark (E2 9C 3F)';      Search = @(0xE2,0x9C,0x3F);                Replace = @(0xE2,0x9C,0x85) },
    @{ Name = 'section (C2 9D)';            Search = @(0xC2,0x9D);                    Replace = @(0xC2,0xA7) },
    @{ Name = 'diamond (CJK fallback)';     Search = @(0xE9,0xA6,0x83,0xE6,0x95,0xB9); Replace = @(0xF0,0x9F,0x94,0xB6) }
)

# cp1252 orphan single-byte -> UTF-8 (only when truly orphan)
$cp1252Singles = @(
    @{ Name = 'em-dash (cp1252 97)'; Byte = 0x97; Replace = @(0xE2,0x80,0x94); IsLead = $false },
    @{ Name = 'section (cp1252 A7)';  Byte = 0xA7; Replace = @(0xC2,0xA7);       IsLead = $false },
    @{ Name = 'times (cp1252 D7)';    Byte = 0xD7; Replace = @(0xC3,0x97);       IsLead = $true  }  # D7 is a valid 2-byte lead
)

# --- Helpers ---------------------------------------------------------------

function Test-IsLeadByte([byte]$b) { $b -ge 0xC0 -and $b -le 0xF7 }
function Test-IsContByte([byte]$b) { $b -ge 0x80 -and $b -le 0xBF }

function Find-CandidateFiles([string]$root, [string[]]$patterns) {
    # Exclude build artifacts, vendored sources, node_modules, .git
    $excludeDirs = @(
        'build', 'build_audit', 'build_dtls_test', 'build_e2e_local',
        'build_esc', 'build_fresh', 'build_linux_test', 'build_ninja',
        'build_probe', 'build_sched', 'build_subagent2', 'build_test',
        'build_verify', 'build_wolfssl_test', 'Testing',
        'src\third_party', 'node_modules', '.git',
        'tests\wolfssl_dtls\node_modules'
    )
    $files = @()
    foreach ($pat in $patterns) {
        $files += Get-ChildItem -Path $root -Recurse -File -Filter $pat -ErrorAction SilentlyContinue
    }
    $files = $files | Where-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\', '/')
        # Universal vendor exclude: any path under a node_modules folder anywhere
        if ($rel -match '(^|[\\/])node_modules([\\/]|$)') { return $false }
        $excluded = $false
        foreach ($ex in $excludeDirs) {
            if ($rel -like "$ex\*" -or $rel -like "$ex/*" -or $rel -eq $ex) {
                $excluded = $true; break
            }
        }
        -not $excluded
    } | Sort-Object -Property FullName -Unique
    return $files
}

function Repair-Bytes([byte[]]$bytes, [ref]$stats) {
    $n = $bytes.Length
    $out = New-Object System.Collections.Generic.List[byte]
    $i = 0
    while ($i -lt $n) {
        $matched = $false

        # Try fixed multi-byte patterns first (longer matches win)
        foreach ($rule in $replacements) {
            $sLen = $rule.Search.Length
            if ($i + $sLen -le $n) {
                $ok = $true
                for ($k = 0; $k -lt $sLen; $k++) {
                    if ($bytes[$i + $k] -ne $rule.Search[$k]) { $ok = $false; break }
                }
                if ($ok) {
                    foreach ($r in $rule.Replace) { $out.Add($r) | Out-Null }
                    $i += $sLen
                    $stats.Value[$rule.Name]++
                    $matched = $true
                    break
                }
            }
        }
        if ($matched) { continue }

        # Try cp1252 single-byte orphans
        $b = $bytes[$i]
        $prev = if ($i -gt 0) { $bytes[$i-1] } else { [byte]0 }
        $next = if ($i+1 -lt $n) { $bytes[$i+1] } else { [byte]0 }
        foreach ($rule in $cp1252Singles) {
            if ($b -ne $rule.Byte) { continue }
            $isOrphan = $false
            if ($rule.IsLead) {
                # D7 is a valid 2-byte UTF-8 lead; only orphan if next is NOT a continuation
                $isOrphan = -not (Test-IsContByte $next)
            } else {
                # 97 / A7 are continuation patterns: orphan iff prev is ASCII or
                # absent (prev < 0x80). The previous byte could be a UTF-8 cont
                # (80-BF), a 2-byte lead (C0-DF), or a 3/4-byte lead (E0-F7) —
                # in all those cases 97/A7 is part of a valid multi-byte
                # sequence and is NOT orphan.
                $isOrphan = ($prev -lt [byte]80)
            }
            if ($isOrphan) {
                foreach ($r in $rule.Replace) { $out.Add($r) | Out-Null }
                $i++
                $stats.Value[$rule.Name]++
                $matched = $true
                break
            }
        }
        if ($matched) { continue }

        # Pass through unchanged
        $out.Add($b) | Out-Null
        $i++
    }
    return ,$out.ToArray()
}

function Test-CleanUtf8([byte[]]$bytes) {
    # Returns $true if the file is perfectly valid UTF-8 with no replacement chars.
    try {
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $hasFFFD = $false
        foreach ($ch in $text.ToCharArray()) {
            if ([int]$ch -eq 0xFFFD) { $hasFFFD = $true; break }
        }
        if ($hasFFFD) { return $false }
        $reEnc = [System.Text.Encoding]::UTF8.GetBytes($text)
        if ($reEnc.Length -ne $bytes.Length) { return $false }
        return $true
    } catch {
        return $false
    }
}

# --- Main ------------------------------------------------------------------

$root = (Resolve-Path $Path).Path
if (-not (Test-Path $root)) {
    Write-Error "Path not found: $root"
    exit 2
}

Write-Host "Scanning under: $root"
Write-Host "Fix mode: $(if ($Fix) { 'ON' } else { 'OFF (read-only)' })"
Write-Host ""

$files = Find-CandidateFiles $root $Pattern
Write-Host "Found $($files.Count) candidate file(s)."
Write-Host ""

$totalFixed = 0
$totalErrors = 0
$totalSkipped = 0
$allStats = @{}

foreach ($file in $files) {
    $rel = $file.FullName.Substring($root.Length).TrimStart('\', '/')
    try {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    } catch {
        Write-Warning "Cannot read $rel : $_"
        $totalErrors++
        continue
    }

    $stats = @{}
    foreach ($rule in ($replacements + $cp1252Singles)) { $stats[$rule.Name] = 0 }

    $repaired = Repair-Bytes $bytes ([ref]$stats)

    $changed = $false
    if ($repaired.Length -ne $bytes.Length) { $changed = $true }
    else {
        for ($k = 0; $k -lt $repaired.Length; $k++) {
            if ($repaired[$k] -ne $bytes[$k]) { $changed = $true; break }
        }
    }

    $fileTotal = 0
    foreach ($k in $stats.Keys) { $fileTotal += $stats[$k] }

    if (-not $changed) {
        # No known corruption. Now do a generic UTF-8 health check.
        if (Test-CleanUtf8 $bytes) {
            if (-not $Quiet) {
                Write-Host "  OK     $rel"
            }
            $totalSkipped++
        } else {
            Write-Host "  DIRTY  $rel  (orphan bytes present, not in known catalog)"
            $totalErrors++
        }
        continue
    }

    # Report what was found
    $report = ($stats.GetEnumerator() | Where-Object { $_.Value -gt 0 } |
        ForEach-Object { "$($_.Value)x $($_.Key)" }) -join ', '
    if ($Fix) {
        # Preserve CRLF if the file originally used it
        $usesCRLF = ($bytes | Where-Object { $_ -eq 0x0A }).Count -lt ($bytes.Length)
        # Detect by counting standalone LF (not preceded by CR)
        $standaloneLF = 0
        for ($k = 0; $k -lt $bytes.Length; $k++) {
            if ($bytes[$k] -eq 0x0A -and ($k -eq 0 -or $bytes[$k-1] -ne 0x0D)) {
                $standaloneLF++
            }
        }
        $usesCRLF = ($standaloneLF -eq 0) -and (($bytes | Where-Object { $_ -eq 0x0A }).Count -gt 0)
        if ($usesCRLF) {
            for ($k = 0; $k -lt $repaired.Length; $k++) {
                if ($repaired[$k] -eq 0x0A -and ($k -eq 0 -or $repaired[$k-1] -ne 0x0D)) {
                    $repaired = $repaired[0..($k-1)] + [byte]0x0D + $repaired[$k..($repaired.Length-1)]
                }
            }
        }
        [System.IO.File]::WriteAllBytes($file.FullName, $repaired)
        Write-Host "  FIXED  $rel  ($report)"
        $totalFixed++
    } else {
        Write-Host "  BROKEN $rel  ($report)"
        $totalErrors++
    }

    foreach ($k in $stats.Keys) {
        if (-not $allStats.ContainsKey($k)) { $allStats[$k] = 0 }
        $allStats[$k] += $stats[$k]
    }
}

Write-Host ""
Write-Host "=== Summary ==="
Write-Host "  Clean files (no changes needed): $totalSkipped"
if ($Fix) {
    Write-Host "  Files repaired:                  $totalFixed"
} else {
    Write-Host "  Files with known corruption:     $totalErrors  (run with -Fix to repair)"
}
if ($allStats.Count -gt 0) {
    Write-Host "  Bytes repaired by category:"
    foreach ($k in ($allStats.Keys | Sort-Object)) {
        if ($allStats[$k] -gt 0) {
            Write-Host "    $($allStats[$k])x  $k"
        }
    }
}

if ($totalErrors -gt 0) { exit 1 } else { exit 0 }
