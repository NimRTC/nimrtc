<#
.SYNOPSIS
    Fast UTF-8 corruption scan + fix using inline C# (.NET Framework compatible).
.DESCRIPTION
    Detects and fixes three families of UTF-8 corruption found in NimRTC docs:
      1. Standalone cp1252 bytes (97, A7, D7) outside any UTF-8 context  -> emit UTF-8 char
      2. cp1252 byte that was a UTF-8 continuation byte of a 3-byte CJK
         character, mistakenly expanded to its UTF-8 representation      -> collapse
      3. Fixed multi-byte sequences with a stray '?' (3F) tail           -> emit proper char
#>

[CmdletBinding()]
param(
    [string]$Path = (Split-Path -Parent $PSScriptRoot),
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'

$cs = @'
using System;
using System.IO;
using System.Collections.Generic;

public class Cp1252Single {
    public string Name;
    public byte B;
    public byte[] Replace;
    public bool IsLead;
}

public class MultiByteRule {
    public string Name;
    public byte[] Search;
    public byte[] Replace;
}

public class CjkExpansion {
    // UTF-8 expansion of a cp1252 byte that lies in 0x80-0xBF (UTF-8 cont range)
    public byte[] Expansion;     // e.g. C2 A7 for 0xA7
    public byte Restored;        // the original cp1252 byte (0xA7 here)
    public string Name;
}

public static class Utf8Fixer {

    public static Cp1252Single[] Singles = new Cp1252Single[] {
        new Cp1252Single { Name = "em-dash (cp1252 97)",    B = 0x97, Replace = new byte[]{0xE2,0x80,0x94}, IsLead = false },
        new Cp1252Single { Name = "section (cp1252 A7)",    B = 0xA7, Replace = new byte[]{0xC2,0xA7},        IsLead = false },
        new Cp1252Single { Name = "multiplication (cp1252 D7)", B = 0xD7, Replace = new byte[]{0xC3,0x97},   IsLead = true  },
    };

    public static MultiByteRule[] Rules = new MultiByteRule[] {
        new MultiByteRule { Name = "CJK orange diamond fallback",
            Search = new byte[]{0xE9,0xA6,0x83,0xE6,0x95,0xB9},
            Replace = new byte[]{0xF0,0x9F,0x94,0xB6} },
        new MultiByteRule { Name = "em-dash EF BF 3F",
            Search = new byte[]{0xEF,0xBF,0x3F}, Replace = new byte[]{0xE2,0x80,0x94} },
        new MultiByteRule { Name = "em-dash E2 80 3F",
            Search = new byte[]{0xE2,0x80,0x3F}, Replace = new byte[]{0xE2,0x80,0x94} },
        new MultiByteRule { Name = "right arrow E2 86 3F",
            Search = new byte[]{0xE2,0x86,0x3F}, Replace = new byte[]{0xE2,0x86,0x92} },
        new MultiByteRule { Name = "check mark E2 9C 3F",
            Search = new byte[]{0xE2,0x9C,0x3F}, Replace = new byte[]{0xE2,0x9C,0x85} },
        new MultiByteRule { Name = "section C2 9D",
            Search = new byte[]{0xC2,0x9D}, Replace = new byte[]{0xC2,0xA7} },
    };

    // CJK expansion catalog: bytes that, when appearing as a CJK UTF-8
    // continuation (0x80-0xBF), were wrongly expanded to their UTF-8 form.
    public static CjkExpansion[] Expansions = new CjkExpansion[] {
        new CjkExpansion { Expansion = new byte[]{0xC2,0xA7},       Restored = 0xA7, Name = "section (cp1252 A7)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x94}, Restored = 0x97, Name = "em-dash (cp1252 97)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x93}, Restored = 0x96, Name = "en-dash (cp1252 96)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xA6}, Restored = 0x85, Name = "ellipsis (cp1252 85)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xA2}, Restored = 0x95, Name = "bullet (cp1252 95)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x9C}, Restored = 0x93, Name = "ldquo (cp1252 93)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x9D}, Restored = 0x94, Name = "rdquo (cp1252 94)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x98}, Restored = 0x91, Name = "lsquo (cp1252 91)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x99}, Restored = 0x92, Name = "rsquo (cp1252 92)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x9A}, Restored = 0x82, Name = "sbquo (cp1252 82)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0x9E}, Restored = 0x84, Name = "bdquo (cp1252 84)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xA0}, Restored = 0x86, Name = "dagger (cp1252 86)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xA1}, Restored = 0x87, Name = "ddagger (cp1252 87)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xB0}, Restored = 0x88, Name = "circ (cp1252 88)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xB9}, Restored = 0x89, Name = "permil (cp1252 89)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xBA}, Restored = 0x9B, Name = "rsaquo (cp1252 9B)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xBB}, Restored = 0x8B, Name = "lsaquo (cp1252 8B)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x82,0xAC}, Restored = 0x80, Name = "euro (cp1252 80)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x84,0xA2}, Restored = 0x99, Name = "trade (cp1252 99)" },
        new CjkExpansion { Expansion = new byte[]{0xE2,0x80,0xA5}, Restored = 0x85, Name = "hellip (cp1252 85)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0xA0},       Restored = 0x8A, Name = "Scaron (cp1252 8A)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0x92},       Restored = 0x8C, Name = "OE (cp1252 8C)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0xBD},       Restored = 0x8E, Name = "Zcaron (cp1252 8E)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0xA1},       Restored = 0x9A, Name = "scaron (cp1252 9A)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0x93},       Restored = 0x9C, Name = "oe (cp1252 9C)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0xBE},       Restored = 0x9E, Name = "zcaron (cp1252 9E)" },
        new CjkExpansion { Expansion = new byte[]{0xC5,0xB8},       Restored = 0x9F, Name = "Ydiaer (cp1252 9F)" },
        new CjkExpansion { Expansion = new byte[]{0xC6,0x92},       Restored = 0x83, Name = "florin (cp1252 83)" },
        new CjkExpansion { Expansion = new byte[]{0xCB,0x9C},       Restored = 0x98, Name = "tilde (cp1252 98)" },
    };

    static bool IsLead2(byte b) { return b >= 0xC0 && b <= 0xDF; }
    static bool IsLead3(byte b) { return b >= 0xE0 && b <= 0xEF; }
    static bool IsLead4(byte b) { return b >= 0xF0 && b <= 0xF7; }
    static bool IsCont(byte b) { return b >= 0x80 && b <= 0xBF; }

    // Try to match an expansion at byte offset 'off' in src. Returns the matched
    // expansion or null if none matches.
    static CjkExpansion MatchExpansion(byte[] src, int off, int n) {
        for (int j = 0; j < Expansions.Length; j++) {
            var e = Expansions[j];
            int len = e.Expansion.Length;
            if (off + len > n) continue;
            bool ok = true;
            for (int k = 0; k < len; k++) {
                if (src[off + k] != e.Expansion[k]) { ok = false; break; }
            }
            if (ok) return e;
        }
        return null;
    }

    public static bool Repair(byte[] src, out byte[] dst, out Dictionary<string,int> stats) {
        stats = new Dictionary<string,int>();
        var list = new List<byte>(src.Length + 64);
        int n = src.Length;
        int i = 0;
        while (i < n) {
            byte b = src[i];

            // ---- Fixed multi-byte rules (must come BEFORE CJK expansion so the
            //      lead byte of patterns like EF BF 3F is preserved for matching) ----
            bool matched = false;
            foreach (var rule in Rules) {
                int sl = rule.Search.Length;
                if (i + sl <= n) {
                    bool ok = true;
                    for (int k = 0; k < sl; k++) {
                        if (src[i + k] != rule.Search[k]) { ok = false; break; }
                    }
                    if (ok) {
                        list.AddRange(rule.Replace);
                        i += sl;
                        if (!stats.ContainsKey(rule.Name)) stats[rule.Name] = 0;
                        stats[rule.Name]++;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) continue;

            // ---- CJK expansion collapse ----
            // Pattern A: CJK_lead (E0-EF) + 1st_cont (80-BF) + <expansion>
            //   The 3rd byte of a CJK char was expanded -> collapse.
            // Pattern B: CJK_lead (E0-EF) + <expansion> + 2nd_cont (80-BF)
            //   The 1st cont was expanded; the 2nd cont is right after -> collapse.
            // Pattern C: CJK_lead (E0-EF) + <expansion> + <expansion> + cont (80-BF)
            //   Both 1st and 2nd conts expanded (rare) -> collapse both.
            if (IsLead3(b)) {
                // ---- Pattern A: lead + 1st_cont (80-BF) + expansion ----
                if (i + 1 < n && IsCont(src[i+1])) {
                    var eA = MatchExpansion(src, i + 2, n);
                    if (eA != null) {
                        list.Add(src[i]);
                        list.Add(src[i+1]);
                        list.Add(eA.Restored);
                        i += 2 + eA.Expansion.Length;
                        if (!stats.ContainsKey("CJK expansion " + eA.Name)) stats["CJK expansion " + eA.Name] = 0;
                        stats["CJK expansion " + eA.Name]++;
                        continue;
                    }
                }

                // ---- Pattern B: lead + expansion + 2nd_cont (80-BF) ----
                if (i + 1 < n) {
                    var eB = MatchExpansion(src, i + 1, n);
                    if (eB != null) {
                        int after = i + 1 + eB.Expansion.Length;
                        if (after < n && IsCont(src[after])) {
                            list.Add(src[i]);
                            list.Add(eB.Restored);
                            // 2nd cont at src[after] will be processed by the main loop
                            i += 1 + eB.Expansion.Length;
                            if (!stats.ContainsKey("CJK expansion " + eB.Name)) stats["CJK expansion " + eB.Name] = 0;
                            stats["CJK expansion " + eB.Name]++;
                            continue;
                        }
                    }
                }

                // ---- Pattern C: lead + expansion + expansion + cont ----
                if (i + 1 < n) {
                    var eBc = MatchExpansion(src, i + 1, n);
                    if (eBc != null) {
                        int afterB = i + 1 + eBc.Expansion.Length;
                        if (afterB < n) {
                            var eC = MatchExpansion(src, afterB, n);
                            if (eC != null) {
                                int afterC = afterB + eC.Expansion.Length;
                                if (afterC < n && IsCont(src[afterC])) {
                                    list.Add(src[i]);
                                    list.Add(eBc.Restored);
                                    list.Add(eC.Restored);
                                    i += 1 + eBc.Expansion.Length + eC.Expansion.Length;
                                    if (!stats.ContainsKey("CJK expansion " + eBc.Name + "+" + eC.Name))
                                        stats["CJK expansion " + eBc.Name + "+" + eC.Name] = 0;
                                    stats["CJK expansion " + eBc.Name + "+" + eC.Name]++;
                                    continue;
                                }
                            }
                        }
                    }
                }

                // No expansion match. Emit just the lead byte and let the next
                // iteration handle the rest. This is important because EF BF 3F
                // (a known multi-byte corruption) starts with a CJK lead + cont
                // but is NOT a CJK char — the multi-byte rule must match it.
                list.Add(b);
                i++;
                continue;
            }

            // ---- Single-byte orphan rules ----
            byte prev = i > 0 ? src[i-1] : (byte)0;
            byte next = i+1 < n ? src[i+1] : (byte)0;
            foreach (var rule in Singles) {
                if (b != rule.B) continue;
                bool isOrphan;
                if (rule.IsLead) {
                    // 2-byte lead (e.g. D7): orphan iff next is NOT a continuation
                    isOrphan = !IsCont(next);
                } else {
                    // Continuation-pattern bytes (97, A7). The previous byte
                    // could be any of:
                    //   - a UTF-8 continuation (80-BF)        -> multi-byte cont (NOT orphan)
                    //   - a 2-byte UTF-8 lead (C0-DF)         -> 97/A7 is its 2nd byte (NOT orphan)
                    //   - a 3-/4-byte UTF-8 lead (E0-F7)       -> part of multi-byte seq (NOT orphan)
                    //   - ASCII (< 0x80) or absent            -> ORPHAN
                    isOrphan = (prev < 0x80);
                }
                if (isOrphan) {
                    list.AddRange(rule.Replace);
                    i++;
                    if (!stats.ContainsKey(rule.Name)) stats[rule.Name] = 0;
                    stats[rule.Name]++;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;

            // Pass-through
            list.Add(b);
            i++;
        }
        dst = list.ToArray();
        if (dst.Length != src.Length) return true;
        for (int k = 0; k < src.Length; k++) {
            if (src[k] != dst[k]) return true;
        }
        return false;
    }

    public static bool IsCleanUtf8(byte[] bytes) {
        int n = bytes.Length;
        int i = 0;
        while (i < n) {
            byte b = bytes[i];
            if (b < 0x80) { i++; continue; }
            int needed;
            if ((b & 0xE0) == 0xC0) needed = 1;
            else if ((b & 0xF0) == 0xE0) needed = 2;
            else if ((b & 0xF8) == 0xF0) needed = 3;
            else return false;
            if (i + needed >= n) return false;
            for (int k = 1; k <= needed; k++) {
                if ((bytes[i+k] & 0xC0) != 0x80) return false;
            }
            i += needed + 1;
        }
        return true;
    }
}
'@

Add-Type -TypeDefinition $cs -Language CSharp

$excludeDirs = @(
    'build', 'build_audit', 'build_dtls_test', 'build_e2e_local',
    'build_esc', 'build_fresh', 'build_linux_test', 'build_ninja',
    'build_probe', 'build_sched', 'build_subagent2', 'build_test',
    'build_verify', 'build_wolfssl_test', 'Testing',
    'src\third_party', 'node_modules', '.git',
    'tests\wolfssl_dtls\node_modules'
)

$patterns = @('*.md', '*.txt')
$root = (Resolve-Path $Path).Path
Write-Host "Scanning under: $root"
Write-Host "Fix mode: $(if ($Fix) { 'ON' } else { 'OFF (read-only)' })"

$files = @()
foreach ($pat in $patterns) {
    $files += @(Get-ChildItem -Path $root -Recurse -File -Filter $pat -ErrorAction SilentlyContinue)
}
$files = $files | Where-Object {
    $rel = $_.FullName.Substring($root.Length).TrimStart('\','/')
    if ($rel -match '(^|[\\/])node_modules([\\/]|$)') { return $false }
    $excluded = $false
    foreach ($ex in $excludeDirs) {
        if ($rel -like "$ex\*" -or $rel -like "$ex/*" -or $rel -eq $ex) {
            $excluded = $true; break
        }
    }
    -not $excluded
} | Sort-Object -Property FullName -Unique

Write-Host "Found $($files.Count) candidate file(s)."
Write-Host ""

$totalFixed = 0
$totalSkipped = 0
$totalDirty = 0
$totalErrors = 0
$allStats = @{}

foreach ($file in $files) {
    $rel = $file.FullName.Substring($root.Length).TrimStart('\','/')
    try {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    } catch {
        Write-Warning "Cannot read $rel"
        $totalErrors++
        continue
    }

    $dst = $null
    $stats = [System.Collections.Generic.Dictionary[string,int]]::new()
    $changed = [Utf8Fixer]::Repair($bytes, [ref]$dst, [ref]$stats)

    if (-not $changed) {
        if ([Utf8Fixer]::IsCleanUtf8($bytes)) {
            $totalSkipped++
            continue
        } else {
            Write-Host "  DIRTY  $rel  (orphan bytes present, not in known catalog)"
            $totalDirty++
            continue
        }
    }

    $report = ''
    $first = $true
    foreach ($k in ($stats.Keys | Sort-Object)) {
        if ($stats[$k] -gt 0) {
            if (-not $first) { $report += ', ' }
            $report += "$($stats[$k])x $k"
            $first = $false
        }
    }

    if ($Fix) {
        $standaloneLF = 0
        $totalLF = 0
        for ($k = 0; $k -lt $bytes.Length; $k++) {
            if ($bytes[$k] -eq 0x0A) { $totalLF++ }
            if ($bytes[$k] -eq 0x0A -and ($k -eq 0 -or $bytes[$k-1] -ne 0x0D)) {
                $standaloneLF++
            }
        }
        $usesCRLF = ($standaloneLF -eq 0) -and ($totalLF -gt 0)
        $out = $dst
        if ($usesCRLF) {
            $tmp = New-Object 'System.Collections.Generic.List[byte]' ($dst.Length + 16)
            for ($k = 0; $k -lt $dst.Length; $k++) {
                if ($dst[$k] -eq 0x0A -and ($k -eq 0 -or $dst[$k-1] -ne 0x0D)) {
                    $tmp.Add(0x0D)
                }
                $tmp.Add($dst[$k])
            }
            $out = $tmp.ToArray()
        }
        [System.IO.File]::WriteAllBytes($file.FullName, $out)
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
    if ($totalDirty -gt 0) {
        Write-Host "  Files with unknown dirty bytes:  $totalDirty"
    }
} else {
    Write-Host "  Files with known corruption:     $totalErrors  (run with -Fix to repair)"
    if ($totalDirty -gt 0) {
        Write-Host "  Files with unknown dirty bytes:  $totalDirty"
    }
}
if ($allStats.Count -gt 0) {
    Write-Host "  Bytes repaired by category:"
    foreach ($k in ($allStats.Keys | Sort-Object)) {
        if ($allStats[$k] -gt 0) {
            Write-Host "    $($allStats[$k])x  $k"
        }
    }
}
if (($totalErrors + $totalDirty) -gt 0) { exit 1 } else { exit 0 }