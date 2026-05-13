# tm-kill.ps1 — canonical Windows kill recipe for tm-hb.sh / tm-wait.sh
# background shells launched by task-loop.
#
# Why this script exists: Claude Code's TaskStop tool (and /bashes -> X)
# only fires TerminateProcess on the outermost tracked bash.exe PID. The
# task-loop shells run as grandchildren under two bash wrappers (the
# snapshot-source wrapper, then an eval bash), so TaskStop's kill misses
# the actual script and it keeps pinging tm-server forever. See
# task-kill/SKILL.md for the full failure-mode write-up.
#
# Usage:
#   tm-kill.ps1 -Mode <mode> -AgentId <id> [-TaskId <tid>]
#
#   Modes:
#     by-task         kill heartbeat(s) for a single task (needs -TaskId)
#     all-for-agent   kill every tm-hb/tm-wait for this agent
#     verify          enumerate; exit 0 if clean, 1 if residual PIDs

param(
    [Parameter(Mandatory=$true)][ValidateSet('by-task','all-for-agent','verify')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$AgentId,
    [string]$TaskId
)

function Find-TmProcs {
    param([string]$AgentId, [string]$TaskId)
    # Get-WmiObject is the only cmdlet variant that reliably returns
    # CommandLine on Win PowerShell 5.1 for cross-session processes.
    # Get-Process .CommandLine is null; Get-CimInstance + -Where can
    # return empty for processes started in other Claude sessions.
    $procs = Get-WmiObject Win32_Process -Filter "Name='bash.exe'" |
        Where-Object {
            $_.CommandLine -ne $null -and
            $_.CommandLine -match 'tm-hb\.sh|tm-wait\.sh' -and
            $_.CommandLine -match [regex]::Escape($AgentId)
        }
    if ($TaskId) {
        # Match the literal TASK_ID="t/NNNN" assignment embedded by the
        # task-loop preamble. The backtick escapes the double-quote
        # inside the PowerShell single-quoted regex string.
        $needle = 'TASK_ID=`"' + [regex]::Escape($TaskId) + '`"'
        $procs = $procs | Where-Object { $_.CommandLine -match $needle }
    }
    return @($procs)
}

$procs = Find-TmProcs -AgentId $AgentId -TaskId $(if ($Mode -eq 'by-task') { $TaskId } else { $null })

switch ($Mode) {
    'verify' {
        if ($procs.Count -eq 0) {
            Write-Output "verified clean: no tm-* bash for $AgentId"
            exit 0
        } else {
            Write-Output "RESIDUAL: $($procs.Count) tm-* bash for $AgentId still alive:"
            $procs | Select-Object ProcessId, CommandLine | Format-List | Out-String | Write-Output
            exit 1
        }
    }
    default {
        if ($Mode -eq 'by-task' -and -not $TaskId) {
            Write-Error "by-task mode requires -TaskId"
            exit 2
        }
        $killed = @()
        foreach ($p in $procs) {
            try {
                Stop-Process -Id $p.ProcessId -Force -Confirm:$false -ErrorAction Stop
                $killed += $p.ProcessId
            } catch {
                # already gone or access denied — fine, continue
            }
        }
        if ($killed.Count -eq 0) {
            Write-Output "killed 0 (no matching processes for $AgentId$(if ($TaskId) { " / $TaskId" }))"
        } else {
            Write-Output "killed $($killed.Count): $($killed -join ',')"
        }
        exit 0
    }
}
