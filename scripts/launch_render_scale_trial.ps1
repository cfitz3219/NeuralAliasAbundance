[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Profile,
    [switch]$CheckOnly,
    [switch]$RestoreOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'render_scale_preference.ps1')
. (Join-Path $PSScriptRoot '../tools/analysis/native_scale_frames.ps1')
$p = Get-Content -LiteralPath $Profile -Raw | ConvertFrom-Json
$culture = [Globalization.CultureInfo]::InvariantCulture
$target = [decimal]$p.Scale
if ($target -lt 0.25 -or $target -ge 1 -or $p.Width -le 0 -or $p.Height -le 0) { throw 'Invalid trial dimensions.' }
$folder = [IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $Profile).Path)
$stateFile = Join-Path $folder 'scale-restore.json'
$prefs = if ([IO.Path]::IsPathRooted($p.Preference)) { $p.Preference } else { Join-Path ([Environment]::GetFolderPath('MyDocuments')) $p.Preference }
$exe = (Resolve-Path -LiteralPath $p.Executable).Path
$binaryDir = [IO.Path]::GetDirectoryName($exe)
$hostIni = Join-Path $binaryDir 'NeuralAliasHost.ini'
$logRoot = [Environment]::ExpandEnvironmentVariables($p.LogFolder)
$out = "$($p.Width)x$($p.Height)"
$expectedW = [math]::Round($p.Width * $target)
$expectedH = [math]::Round($p.Height * $target)
function Get-TrialEdits {
    New-ScaleEdit $prefs $p.Field ($target.ToString('0.000000',$culture))
    # On the tested host this also suppresses the finder/search; native DX11 draw telemetry remains available.
    New-ScaleEdit $hostIni @{ Format='ini'; Section='j2'; Key='enabled' } '0'
}
if ($CheckOnly) {
    foreach ($e in @(Get-TrialEdits)) { Write-Output "$($e.Field.Key) $($e.Original) -> $($e.Request)" }
    Write-Output "Expected world and depth $($expectedW)x$expectedH on $out. Read-only preflight."
    return
}

# Refuse to edit a running game's startup preferences, even during recovery.
function Assert-Closed {
    if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) {
        throw 'The game is running. Close it before starting or restoring this trial.'
    }
}
function Restore-Trial {
    $entries = @(Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json)
    foreach ($e in $entries) {
        # Recovery is confined to the two fields named by the selected profile.
        $isPreference = ($e.Path -eq $prefs -and $e.Field.Format -eq $p.Field.Format -and $e.Field.Key -eq $p.Field.Key -and $e.Field.Section -eq $p.Field.Section)
        $isHost = ($e.Path -eq $hostIni -and $e.Field.Format -eq 'ini' -and $e.Field.Section -eq 'j2' -and $e.Field.Key -eq 'enabled')
        if (!$isPreference -and !$isHost) { throw 'Recovery journal does not match this trial.' }
    }
    foreach ($e in $entries) { Write-ScaleField $e $true }
    [IO.File]::Delete($stateFile)
}
$lock = [IO.File]::Open((Join-Path $folder 'scale-session.lock'), 'OpenOrCreate','ReadWrite','None')
try {
    Assert-Closed
    if ([IO.File]::Exists($stateFile)) { Restore-Trial }
    if ($RestoreOnly) { return }
    $edits = @(Get-TrialEdits)
    if ([decimal]::Parse($edits[0].Original,$culture) -eq $target) { throw 'The original scale already equals the trial. No causal contrast.' }
    if (!(Test-Path -LiteralPath $logRoot -PathType Container)) { throw "Missing native telemetry directory $logRoot" }
    if ($p.HostHash) {
        $dllHash = (Get-FileHash -LiteralPath (Join-Path $binaryDir 'dxgi.dll') -Algorithm SHA256).Hash
        if ($dllHash -ne $p.HostHash) { throw 'The installed recorder differs from the one this trial qualifies.' }
    }
    $draft = $stateFile + '.new'
    [IO.File]::WriteAllText($draft,(ConvertTo-Json -InputObject $edits -Depth 4))
    [IO.File]::Move($draft,$stateFile)
    try {
        foreach ($e in $edits) { Write-ScaleField $e $false }
        Write-Host "Opening the game at $($target * 100)% world scale. Enter the map, play briefly, then quit normally."
        $started = [datetime]::UtcNow
        $start = @{ FilePath = $exe; WorkingDirectory = $binaryDir; PassThru = $true }
        if ($p.Arguments) { $start.ArgumentList = @($p.Arguments) }
        $child = Start-Process @start
        $child.WaitForExit()
        Assert-Closed
    } finally {
        # If the launcher was interrupted while the child is alive, the next invocation restores the journal.
        Assert-Closed
        Restore-Trial
    }
    $log = Join-Path $logRoot ("status-" + $child.Id + ".log")
    $report = Join-Path $folder 'last-scale-result.txt'
    $verdict = 'INCONCLUSIVE: no complete game frame with the selected scene, depth and later screen.'
    $lines = @()
    if ([IO.File]::Exists($log) -and [IO.File]::GetLastWriteTimeUtc($log) -ge $started) {
        $frames = @(Get-ScaleFrames $log)
        $good = @($frames | Where-Object {
            $size = $_.Scene.Split('x')
            $_.Screen -eq $out -and [math]::Abs([int]$size[0] - $expectedW) -le 1 -and [math]::Abs([int]$size[1] - $expectedH) -le 1
        })
        $native = @($frames | Where-Object { $_.Scene -eq $out -and $_.Screen -eq $out })
        if ($good.Count) { $verdict = "OBSERVED: $($good.Count) complete sampled frame(s) at the requested world size on $out." }
        elseif ($native.Count) { $verdict = "REQUEST NOT OBSERVED: $($native.Count) sampled world frame(s) remained at $out." }
        $lines = @($frames | ForEach-Object { "frame $($_.Frame) world/depth $($_.Scene) screen $($_.Screen)" })
    }
    $record = @($verdict,"request $target, PID $($child.Id), exit $($child.ExitCode), source $log") + $lines
    [IO.File]::WriteAllLines($report,[string[]]$record)
    Write-Output $verdict
    Write-Output $report
} finally { $lock.Dispose() }
