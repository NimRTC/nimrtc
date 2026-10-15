<#
.SYNOPSIS
    Clean MSVC build artifacts under D:\MyOpen\NimRTC\build.

.DESCRIPTION
    Removes transient MSVC build-system pollution (CMakeFiles, *.tlog, *.lib.recipe,
    *.ilk, vcpkg.applocal.log, per-test cmake plumbing, etc.) without disturbing
    real working-tree artifacts (CMakeCache.txt, *.lib, *.pdb, *.obj, *.exe).

    By default the script PRESERVES real artifacts so an incremental build can
    resume. Pass -Force to additionally wipe the working tree of real artifacts
    ("nuke the build dir" use case).

    The script intentionally ignores:
      - build/_final_cleanup.ps1 and any build/_*.ps1 helper scripts
        (they belong to other agents / one-shot sessions and must survive).

    All deletions use -LiteralPath + -ErrorAction SilentlyContinue so a missing
    path never throws. A short report is emitted at exit.

.EXAMPLE
    pwsh -File scripts\clean_build.ps1
    # Default: removes MSVC cruft, preserves *.lib/*.pdb/*.obj/*.exe

.EXAMPLE
    pwsh -File scripts\clean_build.ps1 -Force
    # Also removes real artifacts (CMakeCache.txt stays so a future reconfigure
    # still picks up the prior tree, but *.lib / *.exe / *.pdb / *.obj are wiped).

.NOTES
    Run from the repo root, or anywhere — paths are anchored to the script parent.
    Compatible with PowerShell 5.1 (Windows PowerShell) and PowerShell 7+.
#>

[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Continue'

# Resolve repo root from script location (works regardless of cwd).
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Resolve-Path (Join-Path $ScriptDir '..')
$BuildDir    = Join-Path $RepoRoot 'build'

if (-not (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "Nothing to do: $BuildDir does not exist."
    exit 0
}

# Counts for the summary
$script:DirsDeleted  = 0
$script:FilesDeleted = 0
$script:Skipped      = @()

# Skip-list: paths that must never be removed by this script.
# Populated AFTER BuildDir is known but BEFORE any deletions run.
$SkipLiterals = @(
    (Join-Path $BuildDir '_final_cleanup.ps1')
)
# Also skip any other build/_*.ps1 scripts (other agents' helpers).
Get-ChildItem -Path $BuildDir -Filter '_*.ps1' -File -ErrorAction SilentlyContinue |
    ForEach-Object { $SkipLiterals += $_.FullName }

function Remove-Tree {
    param([Parameter(Mandatory)][string]$LiteralPath)
    if (-not (Test-Path -LiteralPath $LiteralPath)) { return }
    foreach ($s in $SkipLiterals) {
        if ($LiteralPath -ieq $s) {
            $script:Skipped += $LiteralPath
            return
        }
    }
    $count = (Get-ChildItem -LiteralPath $LiteralPath -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object).Count
    Remove-Item -LiteralPath $LiteralPath -Recurse -Force -ErrorAction SilentlyContinue
    $script:DirsDeleted++
    Write-Host ("  removed tree : {0} ({1} entries)" -f $LiteralPath, $count)
}

function Remove-Files {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Filter,
        [string]$Mode = 'File'
    )
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $items = @(Get-ChildItem -Path $Path -Recurse -Force -Filter $Filter -ErrorAction SilentlyContinue |
               Where-Object { if ($Mode -eq 'Dir') { $_.PSIsContainer } else { -not $_.PSIsContainer } } |
               Select-Object -ExpandProperty FullName)
    foreach ($item in $items) {
        if (Test-Path -LiteralPath $item) {
            Remove-Item -LiteralPath $item -Force -ErrorAction SilentlyContinue
            $script:FilesDeleted++
        }
    }
    if ($items.Count -gt 0) {
        Write-Host ("  removed {0,5} {1,-22} files in {2}" -f $items.Count, $Filter, $Path)
    }
}

Write-Host ""
Write-Host ("=== clean_build.ps1 : {0} ===" -f $BuildDir)
$modeStr = if ($Force) { 'FORCE (also wiping *.lib / *.pdb / *.obj / *.exe)' } else { 'default (preserves real artifacts)' }
Write-Host ("Mode: {0}" -f $modeStr)
Write-Host ""

# ── 1. Top-level CMake plumbing ────────────────────────────────────────────
Write-Host "[1/6] Top-level CMake plumbing"
Remove-Tree -LiteralPath (Join-Path $BuildDir 'CMakeFiles')
Remove-Item -LiteralPath (Join-Path $BuildDir 'cmake_targets.log') -Force -ErrorAction SilentlyContinue

# ── 2. Per-target CMakeFiles under src/, _vendor_*/, examples/ ────────────
Write-Host ""
Write-Host "[2/6] Per-target CMakeFiles (src/, _vendor_*, examples/)"
$cmakeFilesDirs = @()
$cmakeFilesDirs += Get-ChildItem -Path (Join-Path $BuildDir 'src')      -Recurse -Force -Filter 'CMakeFiles' -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
$vendorRoot = Join-Path $BuildDir '_vendor_'
$cmakeFilesDirs += Get-ChildItem -Path $BuildDir -Recurse -Force -Filter 'CMakeFiles' -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName.StartsWith($vendorRoot, [System.StringComparison]::OrdinalIgnoreCase) } |
    Select-Object -ExpandProperty FullName
$cmakeFilesDirs += Get-ChildItem -Path (Join-Path $BuildDir 'examples') -Recurse -Force -Filter 'CMakeFiles' -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
foreach ($d in ($cmakeFilesDirs | Sort-Object -Unique)) { Remove-Tree -LiteralPath $d }

# ── 3. Debug MSVC transient artifacts ──────────────────────────────────────
Write-Host ""
Write-Host "[3/6] Debug MSVC transient artifacts"
Remove-Files -Path $BuildDir -Filter '*.tlog'            -Mode Dir
Remove-Files -Path $BuildDir -Filter '*.lib.recipe'      -Mode File
Remove-Files -Path $BuildDir -Filter '*.lastbuildstate'  -Mode File
Remove-Files -Path $BuildDir -Filter 'Cl.items.tlog'     -Mode File
Remove-Files -Path $BuildDir -Filter '*.ilk'             -Mode File
Remove-Files -Path $BuildDir -Filter 'vcpkg.applocal.log' -Mode File

# ── 4. Per-test cmake plumbing ─────────────────────────────────────────────
Write-Host ""
Write-Host "[4/6] Per-test cmake plumbing"
Remove-Files -Path $BuildDir -Filter '*[1]_include.cmake'
Remove-Files -Path $BuildDir -Filter '*[1]_tests.cmake'
Remove-Files -Path $BuildDir -Filter 'cmake_install.cmake'

# ── 5. MSVC ZERO_CHECK scaffold under x64/ ─────────────────────────────────
Write-Host ""
Write-Host "[5/6] MSVC x64/ ZERO_CHECK scaffold"
Remove-Tree -LiteralPath (Join-Path $BuildDir 'x64')

# ── 6. P0-A test/build logs at build/ root ──────────────────────────────────
Write-Host ""
Write-Host "[6/6] P0-A top-level build/test logs"
foreach ($logName in @('rebuild_engine.log','rebuild_engine2.log',
                       'rebuild_test_plugin.log','rebuild_more_tests.log',
                       'rebuild_full.log','rebuild_e2e.log')) {
    $p = Join-Path $BuildDir $logName
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
        $script:FilesDeleted++
        Write-Host ("  removed        : {0}" -f $p)
    }
}

# ── Optional: wipe real artifacts when -Force is set ────────────────────────
if ($Force) {
    Write-Host ""
    Write-Host "[FORCE] wiping *.lib / *.pdb / *.obj / *.exe (real artifacts)"
    Remove-Files -Path $BuildDir -Filter '*.lib'
    Remove-Files -Path $BuildDir -Filter '*.pdb'
    Remove-Files -Path $BuildDir -Filter '*.obj'
    Remove-Files -Path $BuildDir -Filter '*.exe'
    # gtest pdbs under build/lib/Debug/
    Get-ChildItem -Path (Join-Path $BuildDir 'lib\Debug') -Filter '*.pdb' -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
            $script:FilesDeleted++
        }
}

# ── Summary ─────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Done ==="
Write-Host ("  dirs  removed : {0}" -f $script:DirsDeleted)
Write-Host ("  files removed : {0}" -f $script:FilesDeleted)
if ($script:Skipped.Count -gt 0) {
    Write-Host ("  skipped (preserved): {0}" -f ($script:Skipped -join ', '))
}
Write-Host  "  preserved     : CMakeCache.txt, src\**\Debug\*.lib, src\**\Debug\*.pdb,"
Write-Host  "                  src\**\Debug\*.obj, tests\Debug\*.exe, build/_*.ps1"
Write-Host ""