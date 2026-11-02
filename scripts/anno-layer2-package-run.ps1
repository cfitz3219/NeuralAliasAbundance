# NeuralAlias automatic reduced-scene Anno test. Reuses the established Anno manifest/rollback runner.
param([ValidateSet('Deploy','Rollback','Results')] [string] $Action = 'Deploy',
    [string] $GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Anno 1800\Bin\Win64',
    [string] $LocalRoot = 'C:\Users\Cort\NeuralAliasAbundance\local',
    [string] $DocumentsDirectory = (Join-Path $env:USERPROFILE 'Documents'))
$ErrorActionPreference = 'Stop'
$Docs     = $DocumentsDirectory
$Log      = Join-Path $Docs 'NA-anno-layer2-log.txt'
$TestCommit = '78d7f609176f6544f1bad0826d33c6d27fc6fb21'

$Local    = $LocalRoot
$Evid     = Join-Path $Local 'j2-anno-flow'
$Manifest = Join-Path $Evid 'manifest.json'
$OldManifest = Join-Path $Local 'j2-anno-dlaa\manifest.json'
function Log([string] $m) { $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m; Write-Host $line; Add-Content -LiteralPath $Log -Value $line }
function Save-Results([string] $why) {
    $logs = @(Get-ChildItem -LiteralPath $Evid -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(status|startup)(-\d+)?\.log$' -or $_.Name -eq 'discovery.jsonl' })
    if (-not $logs) { Log "No run logs to save ($why)."; return $null }
    $dst = Join-Path $Docs ('NA-anno-results-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($f in $logs) { Copy-Item -LiteralPath $f.FullName -Destination $dst }
    foreach ($n in 'NeuralAliasHost.ini', 'manifest.json') { $p = Join-Path $Evid $n; if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst } }
    $summary = New-Object System.Collections.Generic.List[string]
    $summary.Add("NeuralAlias Anno automatic reduced-scene results ($why), $(Get-Date -Format o)")
    $m = if (Test-Path -LiteralPath $Manifest) { Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json } else { $null }
    $summary.Add("Build: $(if ($m) { $m.sourceCommit } else { 'unknown' })")
    foreach ($s in @($logs | Where-Object { $_.Name -like 'status*' })) {
        $lines = Get-Content -LiteralPath $s.FullName
        $summary.Add('')
        $summary.Add("== $($s.Name): $($lines.Count) lines")
        foreach ($p in '^\d\d:\d\d:\d\d fps=', '^j2 on=', '^observe_order', '^j2_misses', '^layer2 ', '^j2flow ') { $last = $lines | Where-Object { $_ -match $p } | Select-Object -Last 1; if ($last) { $summary.Add('last: ' + $(if ($last.Length -gt 400) { $last.Substring(0, 400) + '...' } else { $last })) } }
    }
    $d = Join-Path $Evid 'discovery.jsonl'
    if (Test-Path -LiteralPath $d) { $summary.Add(''); $summary.Add(('Discovery log: discovery.jsonl, {0:N0} bytes (copied)' -f (Get-Item -LiteralPath $d).Length)) }
    $since = [DateTime]::MinValue
    if ($m -and $m.createdUtc) { $c = $m.createdUtc; $since = if ($c -is [DateTime]) { $c.ToUniversalTime() } else { [DateTime]::Parse($c, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime() } }
    $maps = @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'NeuralAlias') -File -Filter 'anno-stage-m-*.txt' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -ge $since })
    foreach ($mp in $maps) {
        Copy-Item -LiteralPath $mp.FullName -Destination $dst
        $ml = Get-Content -LiteralPath $mp.FullName
        $hdr = @($ml | Where-Object { $_ -like 'stage_m: output*' })
        $sc = @($ml | Where-Object { $_ -match 'STRETCH' })
        $summary.Add('')
        $summary.Add("== Map $($mp.Name): $($hdr.Count) frame map(s), $($sc.Count) STRETCH CANDIDATE line(s)")
        foreach ($x in @($ml | Where-Object { $_ -like 'stage_m:*' })) { $summary.Add('  ' + $(if ($x.Length -gt 300) { $x.Substring(0, 300) + '...' } else { $x })) }
    }
    if (-not $maps) { $summary.Add(''); $summary.Add('No optional native frame map; current test evidence is in layer2 and j2 status lines.') }
    $summary | Set-Content -LiteralPath (Join-Path $dst 'summary.txt') -Encoding UTF8
    Log "Results saved to $dst"
    return $dst
}
function Invoke-Rollback([string] $manifestPath) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { Log "No deployment recorded in $manifestPath; nothing to roll back."; return }
    $evidDir = Split-Path -Parent $manifestPath
    $m = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($m.restoredUtc) { Log "Already rolled back ($([IO.Path]::GetFileName($evidDir)))."; return }
    foreach ($f in $m.files) {
        if ([IO.Path]::GetFileName($f.path) -eq 'NeuralAliasHost.ini') { continue } # menu settings are saved into it
        if ($f.deployedHash -and (Test-Path -LiteralPath $f.path) -and (Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) {
            if ($f.existed) { throw "Changed since deploy, left untouched: $($f.path)" }
            $copy = Join-Path $evidDir ('replaced-' + [IO.Path]::GetFileName($f.path))
            Copy-Item -LiteralPath $f.path -Destination $copy -Force
            Log "Note: $([IO.Path]::GetFileName($f.path)) was swapped after our install; a copy is kept at $copy and it is removed like ours."
        }
    }
    foreach ($f in $m.files) {
        if ([IO.Path]::GetFileName($f.path) -eq 'NeuralAliasHost.ini' -and (Test-Path -LiteralPath $f.path)) { Copy-Item -LiteralPath $f.path -Destination (Join-Path $evidDir 'NeuralAliasHost-saved.ini') -Force }
        if ($f.existed) { Copy-Item -LiteralPath $f.backup -Destination $f.path -Force }
        elseif (Test-Path -LiteralPath $f.path) {
            if (-not $f.deployedHash) { Copy-Item -LiteralPath $f.path -Destination (Join-Path $evidDir ([IO.Path]::GetFileName($f.path))) -Force }
            Remove-Item -LiteralPath $f.path
        }
    }
    if ($m.binSnapshot) {
        $now = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
        $diff = Compare-Object -ReferenceObject @($m.binSnapshot) -DifferenceObject $now
        if ($diff) { Log ("WARNING: the game folder differs from before the install: " + (($diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; ')) }
        else { Log 'Game folder matches its pre-install snapshot exactly.' }
    }
    $m | Add-Member -NotePropertyName restoredUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $m | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    Log "Rollback verified ($([IO.Path]::GetFileName($evidDir))): Anno is back to its original files."
}
if ($Action -eq 'Deploy') { Set-Content -LiteralPath $Log -Value "NeuralAlias Anno automatic reduced-scene test $TestCommit, $(Get-Date -Format o)" }
try {
    if ($Action -eq 'Results') {
        $r = Save-Results 'on request'
        if ($r) { Write-Host "`nResults saved to $r" -ForegroundColor Green }
        exit 0
    }
    if (Get-Process -Name 'Anno1800' -ErrorAction SilentlyContinue) { throw 'Anno 1800 is running. Save and exit first.' }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'Anno1800.exe'))) { throw "Anno 1800 not found at $GameDir. Nothing changed." }
    if ($Action -eq 'Rollback') {
        [void](Save-Results 'before rollback')
        Invoke-Rollback $Manifest
        exit 0
    }
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
    if ($package.source_commit -ne $TestCommit) { throw 'Wrong package source; existing installation unchanged.' }
    foreach ($payload in $package.payload) {
        if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $payload.name)).Hash -ne $payload.sha256) { throw "Package mismatch: $($payload.name). Existing installation unchanged." }
    }
    Log "Package verified: commit $TestCommit"
    foreach ($mp in @($OldManifest, $Manifest)) {
        if (Test-Path -LiteralPath $mp) {
            $old = Get-Content -LiteralPath $mp -Raw | ConvertFrom-Json
            if (-not $old.restoredUtc) {
                if ($mp -eq $Manifest) { [void](Save-Results 'previous build, before update') }
                Log "An earlier Anno test build is still installed ($([IO.Path]::GetFileName((Split-Path -Parent $mp)))): rolling it back first (its logs are kept)."
                Invoke-Rollback $mp
            }
        }
    }
    if ([IO.Path]::GetFullPath($Evid) -ne [IO.Path]::GetFullPath((Join-Path $LocalRoot 'j2-anno-flow'))) { throw 'Invalid evidence path.' }
    if (Test-Path -LiteralPath $Evid) { Move-Item -LiteralPath $Evid -Destination ($Evid + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
    New-Item -ItemType Directory -Force -Path $Evid | Out-Null
    foreach ($n in 'dxgi.dll', 'NeuralAliasHost.ini', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt') { if (Test-Path -LiteralPath (Join-Path $GameDir $n)) { throw "$n is still in the Anno folder and no NeuralAlias install record owns it. Nothing changed." } }

    $ini = Join-Path $Evid 'NeuralAliasHost.ini'
    @"
[settings]
scope=anno-layer2
[host]
startup_log=$Evid\startup.log
status_log=$Evid\status.log
enabled=1
tag=ANNO AUTO SCENE TEST
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
[dlss]
enabled=0
[nr]
enabled=0
[motion]
enabled=0
[fg]
enabled=0
[adapter]
enabled=1
[observe]
producers=1
early_install=1
[discovery]
enabled=1
read_only=1
log=$Evid\discovery.jsonl
[j2]
enabled=1
route=auto
scale_mode=0
preset=L
sharpness=0
mip_bias=-1
flow=0
[layer2]
enabled=1
"@ | Set-Content -LiteralPath $ini -Encoding ascii

    $dll = Join-Path $PSScriptRoot 'NeuralAliasDXGI.dll'
    $names = @('dxgi.dll', 'NeuralAliasHost.ini', 'NeuralAliasHost-status.log', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt')
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'nvngx_dlss.dll'))) { $names += 'nvngx_dlss.dll' }
    $files = @()
    foreach ($n in $names) {
        $p = Join-Path $GameDir $n
        $src = switch ($n) { 'dxgi.dll' { $dll } 'nvngx_dlss.dll' { (Join-Path $PSScriptRoot 'nvngx_dlss.dll') } 'NeuralAliasHost.ini' { $ini } 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt' { (Join-Path $PSScriptRoot 'AMD-OpticalFlow-LICENSE.txt') } default { $null } }
        $files += @{ path = $p; existed = $false; backup = $null; source = $src; deployedHash = $(if ($src) { (Get-FileHash -LiteralPath $src).Hash } else { $null }) }
    }
    $snapshot = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
    @{ createdUtc = [DateTime]::UtcNow.ToString('o'); sourceCommit = $TestCommit; package = $PSScriptRoot; binSnapshot = $snapshot; gameExeHash = (Get-FileHash -LiteralPath (Join-Path $GameDir 'Anno1800.exe')).Hash; files = $files } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Manifest
    foreach ($f in $files) {
        if ($f.source) {
            Copy-Item -LiteralPath $f.source -Destination $f.path
            if ((Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) { throw "Deployment hash mismatch: $($f.path)" }
        }
    }
    Log 'READY. Start Anno in DirectX 12, native display resolution, game Anti-Aliasing Off. Home > Picture > Live DLAA > Render mode: DLAA baseline, then Quality.'
    Write-Host "`nREADY. Start Anno (DirectX 12), load a save, press Home." -ForegroundColor Green
    Write-Host 'Picture > Live DLAA: leave the switch ON; Render mode DLAA is the full-resolution baseline. Then choose Quality, close Home and play normally.' -ForegroundColor Green
    Write-Host 'Allow scene preparation to finish. A smaller scene size AND a running DLSS status confirm upscaling. Menus and HUD should stay full size.' -ForegroundColor Green
    Write-Host 'Switch Render mode back to DLAA to compare. No restart is required. Keep other enhancement controls unchanged for the first comparison.' -ForegroundColor Green
    Write-Host 'After Save and Exit: NA-anno-layer2-results.cmd. Undo: NA-anno-layer2-rollback.cmd.' -ForegroundColor Green
} catch {
    Log "STOPPED: $($_.Exception.Message)"
    Write-Host "`nSTOPPED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
