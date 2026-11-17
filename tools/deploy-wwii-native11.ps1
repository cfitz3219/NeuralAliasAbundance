# Reuses the existing WWII additive install/receipt transaction and shared results collector.
param([ValidateSet('Deploy','Rollback','Results')] [string] $Action = 'Deploy',
    [string] $GameDir = 'D:\SteamLibrary\steamapps\common\Call of Duty WWII',
    [string] $DocumentsDirectory = (Join-Path $env:USERPROFILE 'Documents'),
    [string] $EvidenceDirectory = 'C:\Users\Cort\NeuralAliasAbundance\local\j2-wwii-on12')
$ErrorActionPreference = 'Stop'
$Docs = $DocumentsDirectory
$Log = Join-Path $Docs 'NA-wwii-log.txt'
$TestCommit = '0e2261c17a0f47031fb7176795d11010ebacbc58'
$Evid = [IO.Path]::GetFullPath($EvidenceDirectory)
$Manifest = Join-Path $Evid 'manifest.json'
$Keep = @{ sharpness = '100'; preset = 'M'; mip_bias = '-1.0'; flow_loosen = '50' }
function Log([string] $m) { $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m; Write-Host $line; Add-Content -LiteralPath $Log -Value $line }
function Save-Results([string] $why) {
    $logs = @(Get-ChildItem -LiteralPath $Evid -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(status|startup)(-\d+)?\.log$' -or $_.Name -eq 'discovery.jsonl' })
    if (-not $logs) { Log "No run logs to save ($why)."; return $null }
    $dst = Join-Path $Docs ('NA-wwii-results-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($f in $logs) { Copy-Item -LiteralPath $f.FullName -Destination $dst }
    foreach ($n in 'NeuralAliasHost.ini', 'manifest.json') { $p = Join-Path $Evid $n; if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst } }
    $summary = New-Object System.Collections.Generic.List[string]
    $summary.Add("NeuralAlias WWII campaign native DX11 DLAA results ($why), $(Get-Date -Format o)")
    $m = if (Test-Path -LiteralPath $Manifest) { Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json } else { $null }
    $summary.Add("Build: $(if ($m) { $m.sourceCommit } else { 'unknown' })")
    foreach ($s in @($logs | Where-Object { $_.Name -like 'status*' })) {
        $lines = Get-Content -LiteralPath $s.FullName
        $summary.Add('')
        $summary.Add("== $($s.Name): $($lines.Count) lines")
        $patterns = '^native11 ', '^native11_dlaa ', '^native11_scene ', '^native11_inputs ', '^dx11on12', '^j2 ', '^j2_summary', '^j2_misses', '^j2_reasons', '^j2_camera', '^\d\d:\d\d:\d\d fps='
        foreach ($pattern in $patterns) {
            $last = $lines | Where-Object { $_ -match $pattern } | Select-Object -Last 1
            if ($last) { $summary.Add($last) }
        }
    }
    $d = Join-Path $Evid 'discovery.jsonl'
    if (Test-Path -LiteralPath $d) { $summary.Add(''); $summary.Add(('Discovery log: discovery.jsonl, {0:N0} bytes (copied)' -f (Get-Item -LiteralPath $d).Length)) }
    $since = [DateTime]::MinValue
    if ($m -and $m.createdUtc) { $c = $m.createdUtc; $since = if ($c -is [DateTime]) { $c.ToUniversalTime() } else { [DateTime]::Parse($c, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime() } }
    # Captures: only folders this game and this run's processes produced (session.json exe+pid); untagged folders listed apart.
    . (Join-Path $PSScriptRoot 'select-captures.ps1')
    $runPids = @(Get-ChildItem -LiteralPath $Evid -File -Filter 'status-*.log' -ErrorAction SilentlyContinue | ForEach-Object { if ($_.BaseName -match '^status-(\d+)$') { [int]$Matches[1] } })
    $sel = Select-NACaptures -Exe 's2_sp64_ship' -Pids $runPids -Since $since
    $summary.Add('')
    $summary.Add("Captures from this run (s2_sp64_ship, pids $($runPids -join ',')): " + $(if ($sel.Matched) { $sel.Matched.Count } else { 'none' }))
    foreach ($cd in $sel.Matched) {
        $cdst = Join-Path $dst ('captures\' + $cd.Name); New-Item -ItemType Directory -Force -Path $cdst | Out-Null
        Get-ChildItem -LiteralPath $cd.FullName -File | Where-Object { $_.Extension -ne '.raw' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $cdst }
        $summary.Add("  $($cd.Name): metadata copied to captures\$($cd.Name); raw planes stay in $($cd.FullName)")
    }
    if ($sel.Other) { $summary.Add("Other games/processes (not this run, not copied): $($sel.Other.Count)") }
    $summary.Add('Unattributed legacy captures since the install (older builds, no session.json; never counted as this run): ' + $(if ($sel.Legacy) { ($sel.Legacy | ForEach-Object { $_.Name }) -join ', ' } else { 'none' }))
    $summary | Set-Content -LiteralPath (Join-Path $dst 'summary.txt') -Encoding UTF8
    try { & (Join-Path $PSScriptRoot 'summarize-run.ps1') -Results $dst -Exe 's2_sp64_ship' | Out-Null }
    catch { Log "Run summary failed; full logs are saved: $($_.Exception.Message)" }
    Log "Results saved to $dst"
    return $dst
}
function Invoke-Rollback {
    if (-not (Test-Path -LiteralPath $Manifest)) { Log 'No J2 deployment found; nothing to roll back.'; return }
    $m = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
    if ($m.restoredUtc) { Log 'Already rolled back.'; return }
    $owned = 'dxgi.dll', 'nvngx_dlss.dll', 'NeuralAliasHost.ini', 'NeuralAliasHost-status.log', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt'
    foreach ($f in $m.files) {
        $name = [IO.Path]::GetFileName($f.path)
        if ($name -notin $owned -or $f.existed -or [IO.Path]::GetFullPath($f.path) -ne (Join-Path $GameDir $name)) {
            throw 'The installation record does not match this additive WWII installation.'
        }
        # The settings file legitimately changes (menu settings such as Sharpness are saved into it): no hash guard.
        if ([IO.Path]::GetFileName($f.path) -eq 'NeuralAliasHost.ini') { continue }
        if ($f.deployedHash -and (Test-Path -LiteralPath $f.path) -and (Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) {
            throw "Changed since deploy, left untouched: $($f.path)"
        }
    }
    foreach ($f in $m.files) {
        if ([IO.Path]::GetFileName($f.path) -eq 'NeuralAliasHost.ini' -and (Test-Path -LiteralPath $f.path)) {
            Copy-Item -LiteralPath $f.path -Destination (Join-Path $Evid 'NeuralAliasHost-saved.ini') -Force # keeps menu settings for next time
        }
        if ($f.existed) { Copy-Item -LiteralPath $f.backup -Destination $f.path -Force }
        elseif (Test-Path -LiteralPath $f.path) {
            if (-not $f.deployedHash) { Copy-Item -LiteralPath $f.path -Destination (Join-Path $Evid ([IO.Path]::GetFileName($f.path))) -Force }
            Remove-Item -LiteralPath $f.path
        }
    }
    if ($m.binSnapshot) {
        $now = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
        $diff = Compare-Object -ReferenceObject @($m.binSnapshot) -DifferenceObject $now
        if ($diff) { Log ("WARNING: the bin folder differs from before the install: " + (($diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; ')) }
        else { Log 'Bin folder matches its pre-install snapshot exactly.' }
    }
    $m | Add-Member -NotePropertyName restoredUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $m | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Manifest
    Log 'Rollback verified: Call of Duty WWII campaign is back to its original files.'
}
if ($Action -eq 'Deploy') { Set-Content -LiteralPath $Log -Value "NeuralAlias native DX11 WWII campaign DLAA ($TestCommit), $(Get-Date -Format o)" }
try {
    if ($Action -eq 'Results') {
        $r = Save-Results 'on request'
        if ($r) { Write-Host "`nResults saved to $r" -ForegroundColor Green }
        exit 0
    }
    $GameDir = (Resolve-Path -LiteralPath $GameDir).ProviderPath
    if (Get-Process -Name 's2_sp64_ship','s2_mp64_ship' -ErrorAction SilentlyContinue) { throw 'Call of Duty WWII campaign is running. Save and exit first.' }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 's2_sp64_ship.exe'))) { throw "Game not found at $GameDir. Nothing changed." }
    if ($Action -eq 'Rollback') {
        [void](Save-Results 'before rollback')
        Invoke-Rollback
        exit 0
    }
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
    if($package.source_commit -ne $TestCommit) { throw 'Wrong package source; existing installation unchanged.' }
    foreach ($required in 'NeuralAliasDXGI.dll', 'nvngx_dlss.dll', 'AMD-OpticalFlow-LICENSE.txt', 'select-captures.ps1', 'summarize-run.ps1', 'deploy.ps1') {
        if ($required -notin @($package.payload.name)) { throw "Incomplete package: $required. Existing installation unchanged." }
    }
    foreach($payload in $package.payload) {
        if($payload.name -ne [IO.Path]::GetFileName($payload.name) -or (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $payload.name)).Hash -ne $payload.sha256) {
            throw "Package mismatch: $($payload.name). Existing installation unchanged."
        }
    }
    if (Test-Path -LiteralPath $Manifest) {
        $old = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
        if (-not $old.restoredUtc) {
            [void](Save-Results 'previous build, before update')
            Log 'The previous test build is still installed: rolling it back first (its logs are kept).'
            Invoke-Rollback
        }
        # Menu choices saved last time (Sharpness, DLSS preset, Texture detail) carry over.
        $saved = Join-Path $Evid 'NeuralAliasHost-saved.ini'
        if (Test-Path -LiteralPath $saved) {
            $txt = Get-Content -LiteralPath $saved -Raw
            $j2 = [regex]::Match($txt, '(?ms)^\[j2\](.*?)(^\[|\z)').Groups[1].Value
            foreach ($k in 'sharpness', 'mip_bias', 'flow_loosen') {
                $mm = [regex]::Match($j2, "(?m)^$k=([^\r\n]+)")
                if ($mm.Success) { $Keep[$k] = $mm.Groups[1].Value.Trim() }
            }
            Log ("Keeping your menu choices: " + (($Keep.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', '))
        }
        $resolvedEvidence = (Resolve-Path -LiteralPath $Evid).ProviderPath
        if ($resolvedEvidence -ne [IO.Path]::GetFullPath($EvidenceDirectory) -or $resolvedEvidence -eq $GameDir -or $resolvedEvidence -eq [IO.Path]::GetPathRoot($resolvedEvidence)) { throw 'Invalid evidence archive path.' }
        Move-Item -LiteralPath $resolvedEvidence -Destination ($resolvedEvidence + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    }
    New-Item -ItemType Directory -Force -Path $Evid | Out-Null
    foreach ($n in 'dxgi.dll', 'd3d11.dll', 'nvngx_dlss.dll', 'NeuralAliasHost.ini', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt') { if (Test-Path -LiteralPath (Join-Path $GameDir $n)) { throw "$n already exists in the game folder. This script never overwrites game files. Nothing changed." } }

    $commit = $TestCommit
    $dll = Join-Path $PSScriptRoot 'NeuralAliasDXGI.dll'
    $sdkDll = Join-Path $PSScriptRoot 'nvngx_dlss.dll' # bundled, checked before any rollback
    Log "Using native DX11 DLAA build $commit."

    $ini = Join-Path $Evid 'NeuralAliasHost.ini'
    @"
[host]
build_commit=$TestCommit
executable=s2_sp64_ship.exe
startup_log=$Evid\startup.log
status_log=$Evid\status.log
enabled=1
tag=Native DX11 DLAA (Call of Duty WWII campaign)
[dx11on12]
enabled=0
[native11]
enabled=1
[overlay]
enabled=1
[settings]
scope=wwii-on12
[route_a]
enabled=0
[fsr]
enabled=0
replace=0
[xess]
enabled=0
replace=0
[present]
proxy=0
render_small=0
native_hud=0
present_dlaa=0
size_probe=0
[dlss]
enabled=0
[nr]
enabled=0
[motion]
enabled=0
[fg]
enabled=0
[adapter]
enabled=0
[observe]
profile_hooks=0
usage_log=1
producers=0
early_install=0
[discovery]
enabled=0
read_only=1
log=$Evid\discovery.jsonl
[j2]
enabled=1
native_scale=0
draw_replay=0
scale_mode=0
window=0
read_hook=0
flow=0
flow_loosen=$($Keep['flow_loosen'])
mip_bias=$($Keep['mip_bias'])
mip_bias_live=1
sharpness=$($Keep['sharpness'])
preset=$($Keep['preset'])
route=auto
capture_frames=30
capture_auto=0
capture_cooldown_seconds=20
[diagnostics]
camera_inputs=1
[layer2]
enabled=0
"@ | Set-Content -LiteralPath $ini -Encoding ascii
    $files = @()
    $ngxDst = Join-Path $GameDir 'nvngx_dlss.dll'
    $names = @('dxgi.dll', 'NeuralAliasHost.ini', 'NeuralAliasHost-status.log', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt')
    if (Test-Path -LiteralPath $ngxDst) { throw 'The game folder already has an nvngx_dlss.dll; this script never overwrites game files. Nothing changed.' } else { $names += 'nvngx_dlss.dll' }
    foreach ($n in $names) {
        $p = Join-Path $GameDir $n
        $src = switch ($n) { 'dxgi.dll' { $dll } 'NeuralAliasHost.ini' { $ini } 'nvngx_dlss.dll' { $sdkDll } 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt' { (Join-Path $PSScriptRoot 'AMD-OpticalFlow-LICENSE.txt') } default { $null } }
        $files += @{ path = $p; existed = $false; backup = $null; source = $src; deployedHash = $(if ($src) { (Get-FileHash -LiteralPath $src).Hash } else { $null }) }
    }
    $snapshot = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
    @{ createdUtc = [DateTime]::UtcNow.ToString('o'); sourceCommit = $commit; binSnapshot = $snapshot; gameExeHash = (Get-FileHash -LiteralPath (Join-Path $GameDir 's2_sp64_ship.exe')).Hash; files = $files } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Manifest
    $installStarted = $true
    foreach ($f in $files) {
        if ($f.source) {
            [IO.File]::Copy($f.source, $f.path, $false)
            if ((Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) { throw 'Deployment hash mismatch' }
        }
    }
    $installStarted = $false
    Log 'READY. Start Call of Duty WWII campaign from Steam as usual (DirectX 11 as the game normally runs, anti-aliasing off). Play a campaign mission for a few minutes, move the camera around, quit, then run NA-wwii-on12-results.cmd.'
    Write-Host "`nREADY. Start Call of Duty WWII campaign from Steam as usual." -ForegroundColor Green
    Write-Host "Game graphics: leave the game on DirectX 11 (native DX11; the translation bridge stays off), Anti-aliasing OFF (no FXAA/MSAA), 3D resolution scale at 1.0 (full)." -ForegroundColor Green
    Write-Host "In the game press Home and set Picture > Render mode to DLAA. It switches live." -ForegroundColor Yellow
    Write-Host "Preset M and your saved settings are preserved. This build corrects false motion on stationary details." -ForegroundColor Green
    Write-Host "This run is DLAA only. Play a few minutes (campaign only), pan and zoom the camera, open a menu once, then quit." -ForegroundColor Green
    Write-Host "Then run Documents\NA-wwii-on12-results.cmd: it saves the logs to a Documents\NA-wwii-results-... folder." -ForegroundColor Green
    Write-Host "To restore the original game files: Documents\NA-wwii-on12-rollback.cmd (it saves the results first)." -ForegroundColor Green
} catch {
    if ($installStarted) { try { Invoke-Rollback } catch { Log "Partial installation record retained for recovery: $_" } }
    Log "STOPPED: $($_.Exception.Message)"
    Write-Host "`nSTOPPED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}


