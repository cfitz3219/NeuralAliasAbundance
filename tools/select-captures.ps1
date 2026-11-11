# Select capture folders by the game that produced them (Astra/Cort 2026-11-03: every game shares
# LocalAppData\NeuralAlias\captures, so results scripts picked up other games' captures).
# Since the capture_session change each folder has session.json {kind, exe, path, pid, process_started_utc}.
#
#   . .\select-captures.ps1
#   $sel = Select-NACaptures -Exe 'Anno1800' -Pids @(12600) -Since $installTimeUtc
#   $sel.Matched     # folders this game/session produced
#   $sel.Other       # tagged folders from other games or other processes (never copy them as this run's)
#   $sel.Legacy      # untagged folders (older builds): report separately as "unattributed"
function Select-NACaptures {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,   # exe name without extension, as in session.json
        [int[]]$Pids = @(),                           # this run's game pids (from status-<pid>.log names); empty = any pid
        [datetime]$Since = [datetime]::MinValue,      # folders written before this are ignored
        [string]$Root = (Join-Path $env:LOCALAPPDATA 'NeuralAlias\captures')
    )
    $matched = @(); $other = @(); $legacy = @()
    if (-not (Test-Path -LiteralPath $Root)) { return [pscustomobject]@{ Matched = $matched; Other = $other; Legacy = $legacy } }
    foreach ($d in Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue) {
        if ($d.LastWriteTimeUtc -lt $Since.ToUniversalTime()) { continue }
        $tag = Join-Path $d.FullName 'session.json'
        if (-not (Test-Path -LiteralPath $tag)) { $legacy += $d; continue }
        try { $s = Get-Content -LiteralPath $tag -Raw | ConvertFrom-Json } catch { $legacy += $d; continue }
        $sameExe = [string]::Equals([string]$s.exe, $Exe, [System.StringComparison]::OrdinalIgnoreCase)
        $samePid = ($Pids.Count -eq 0) -or ($Pids -contains [int]$s.pid)
        if ($sameExe -and $samePid) { $matched += $d } else { $other += $d }
    }
    [pscustomobject]@{ Matched = $matched; Other = $other; Legacy = $legacy }
}
