<#
.SYNOPSIS
    Register (or remove) the daily Windows soak-evidence Scheduled Task.

.DESCRIPTION
    The v1.1 release gate needs seven consecutive days of green Case D
    and Case E acceptance runs on Windows and on Linux x86_64.  The
    Linux half comes from the scheduled GitHub Actions workflow; the
    Windows half comes from this task, which runs
    `scripts/run-soak-daily.ps1` once a day.

    The task is registered for the current user with an interactive
    logon, so it runs whenever the machine is on and the user is
    logged in.  `StartWhenAvailable` makes a missed trigger fire as
    soon as the machine is next available -- a gap day breaks the
    streak, so a task that never runs at all is worse than one that
    runs late.

    The trigger minute is deliberately not `:00`: every other job on a
    machine tends to be scheduled on the hour, and this one has to
    find the build tree undisturbed.

.EXAMPLE
    .\scripts\register-soak-task.ps1

.EXAMPLE
    .\scripts\register-soak-task.ps1 -Time 03:17

.EXAMPLE
    .\scripts\register-soak-task.ps1 -Remove
#>

[CmdletBinding()]
param(
    # The daily trigger time, local time, as HH:mm.
    [string] $Time = '03:07',
    [string] $TaskName = 'NimRTC soak',
    [string] $RepoPath = '',
    # Unregister the task instead of registering it.
    [switch] $Remove
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = if ($RepoPath) { $RepoPath } else { Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$runner = Join-Path $repo 'scripts\run-soak-daily.ps1'
if (-not (Test-Path -LiteralPath $runner)) {
    Write-Error "daily runner not found: $runner"
    exit 2
}

if ($Remove) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Output "removed: $TaskName"
    }
    else {
        Write-Output "not registered: $TaskName"
    }
    exit 0
}

try {
    $triggerTime = [datetime]::ParseExact($Time, 'HH:mm', $null)
}
catch {
    Write-Error "Time must be HH:mm (got '$Time')"
    exit 2
}

# The window is a statement about the tree that gets tagged.  A day
# measured on another branch is excluded from the window by the
# collector's own rule, so a checkout on the wrong branch wastes a
# day; say so loudly rather than letting it pass unnoticed.
$branch = (& git -C $repo rev-parse --abbrev-ref HEAD 2>$null)
if ($branch -and $branch -ne 'release/1.1') {
    Write-Warning @"
The checkout is on '$branch', not on 'release/1.1'.  Days measured here
will be recorded as 'measured on another tree' and will NOT count towards
the v1.1 window.  Switch the checkout before the task fires.
"@
}

$action = New-ScheduledTaskAction `
    -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$runner`"" `
    -WorkingDirectory $repo

$trigger = New-ScheduledTaskTrigger -Daily -At $triggerTime

$principal = New-ScheduledTaskPrincipal `
    -UserId ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) `
    -LogonType Interactive `
    -RunLevel Limited

$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -DontStopOnIdleEnd `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Hours 2) `
    -RestartCount 2 `
    -RestartInterval (New-TimeSpan -Minutes 15)

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Principal $principal `
    -Settings $settings `
    -Description 'Daily NimRTC v1.1 soak evidence collection (v1.1 plan C5)' `
    -Force | Out-Null

$info = Get-ScheduledTaskInfo -TaskName $TaskName
Write-Output ("registered: {0} (daily at {1}, next run {2})" -f $TaskName, $Time, $info.NextRunTime)
Write-Output "read the last result with:"
Write-Output ("  Get-ScheduledTaskInfo -TaskName '{0}' | Select-Object LastRunTime, LastTaskResult" -f $TaskName)
