# One install / uninstall / results script for Route B. The game is a parameter, never code:
# the caller (a package's .cmd files) passes the folder, the exe name and a short label.
# Additive only: it refuses to overwrite anything the game ships, records what it added, and removes exactly that.
param([ValidateSet('Deploy','Rollback','Results')] [string] $Action = 'Deploy',
    [Parameter(Mandatory = $true)] [string] $GameDir,
    [Parameter(Mandatory = $true)] [string] $Exe,          # e.g. Game.exe, as it sits in $GameDir
    [Parameter(Mandatory = $true)] [string] $Label,        # short name for folders and logs, letters/digits/dashes
    [string] $Title = $Label,
    [string] $DocumentsDirectory = (Join-Path $env:USERPROFILE 'Documents'),
    [string] $EvidenceRoot = (Join-Path $env:LOCALAPPDATA 'NeuralAlias\runs'),
    [ValidateSet('Native11','Auto')] [string] $GraphicsApi = 'Native11',
    [switch] $StartAfterFactory,
    [string[]] $AlsoClose = @())
$ErrorActionPreference = 'Stop'
if ($Label -notmatch '^[A-Za-z0-9-]{1,40}$') { throw 'Label must be letters, digits and dashes.' }
$Docs = $DocumentsDirectory
$Log = Join-Path $Docs "NA-$Label-log.txt"
$Evid = Join-Path ([IO.Path]::GetFullPath($EvidenceRoot)) $Label
$Manifest = Join-Path $Evid 'manifest.json'
$ExeBase = [IO.Path]::GetFileNameWithoutExtension($Exe)
$autoApi = $GraphicsApi -eq 'Auto'
$Owned = 'dxgi.dll', 'nvngx_dlss.dll', 'NeuralAliasDlaa64.exe', 'NeuralAliasHost.ini', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt'
function Log([string] $m) { $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m; Write-Host $line; Add-Content -LiteralPath $Log -Value $line }
# 0x8664 = 64-bit, 0x014c = 32-bit. A proxy of the wrong kind is never loaded by the game, silently.
function Get-PEMachine([string] $path) {
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3c; $pe = $br.ReadInt32()
        $fs.Position = $pe; if ($br.ReadUInt32() -ne 0x4550) { throw "Not a Windows program: $path" }
        return $br.ReadUInt16()
    } finally { $fs.Dispose() }
}
function Bits([int] $machine) { switch ($machine) { 0x8664 { '64-bit' } 0x014c { '32-bit' } default { 'unknown kind 0x{0:x}' -f $machine } } }
function Save-Results([string] $why) {
    $dst = Join-Path $Docs ("NA-$Label-results-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $logs = @(Get-ChildItem -LiteralPath $Evid -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(status|startup)(-\d+)?\.log$' -or $_.Name -eq 'discovery.jsonl' })
    foreach ($f in $logs) { Copy-Item -LiteralPath $f.FullName -Destination $dst }
    foreach ($n in 'NeuralAliasHost.ini', 'manifest.json') { $p = Join-Path $Evid $n; if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dst } }
    $m = if (Test-Path -LiteralPath $Manifest) { Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json } else { $null }
    $summary = New-Object System.Collections.Generic.List[string]
    $summary.Add("NeuralAlias $Title results ($why), $(Get-Date -Format o)")
    $summary.Add("Build: $(if ($m) { $m.sourceCommit } else { 'unknown' })   Program: $(if ($m) { $m.proxyBits } else { 'unknown' })   Game: $(if ($m) { $m.gameBits } else { 'unknown' })")
    if (-not $logs) {
        # Says WHY there is nothing, with the candidates, instead of an empty folder.
        $summary.Add('')
        $summary.Add('NO RUN LOGS. The game never loaded the program. Candidates, most likely first:')
        $summary.Add('  1. The game was not started since the install.')
        $summary.Add('  2. The game is running on DirectX 9 or another API that does not load dxgi.dll (check its graphics settings for a DirectX 11 choice).')
        $summary.Add('  3. The game starts from a different folder or through a launcher exe, so the program sat next to the wrong exe.')
        $present = @($Owned | Where-Object { Test-Path -LiteralPath (Join-Path $GameDir $_) })
        $summary.Add("  Files of ours in the game folder right now: $(if ($present) { $present -join ', ' } else { 'none' })")
    }
    foreach ($s in @($logs | Where-Object { $_.Name -like 'status*' })) {
        $lines = Get-Content -LiteralPath $s.FullName
        $summary.Add(''); $summary.Add("== $($s.Name): $($lines.Count) lines")
        foreach ($pattern in '^native11 ', '^native11_dlaa ', '^native11_scene ', '^native11_inputs ', '^dx11on12', '^overlay', '^j2 ', '^j2_summary', '^j2_misses', '^j2_reasons', '^j2_camera', '^\d\d:\d\d:\d\d fps=') {
            $last = $lines | Where-Object { $_ -match $pattern } | Select-Object -Last 1
            if ($last) { $summary.Add($last) }
        }
        # What the referee found, said plainly (the raw lines stay in the copied log).
        $rs = Join-Path $PSScriptRoot 'referee-summary.ps1'
        if (Test-Path -LiteralPath $rs) { . $rs; foreach ($l in (Get-RefereeSummary $lines)) { $summary.Add($l) } }
    }
    foreach ($s in @($logs | Where-Object { $_.Name -like 'startup*' })) {
        $summary.Add(''); $summary.Add("== $($s.Name) (first 60 lines)")
        Get-Content -LiteralPath $s.FullName -TotalCount 60 | ForEach-Object { $summary.Add($_) }
    }
    $sc = Join-Path $PSScriptRoot 'select-captures.ps1'
    if ($logs -and (Test-Path -LiteralPath $sc)) {
        . $sc
        $since = [DateTime]::MinValue
        if ($m -and $m.createdUtc) { $c = $m.createdUtc; $since = if ($c -is [DateTime]) { $c.ToUniversalTime() } else { [DateTime]::Parse($c, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime() } }
        $runPids = @(Get-ChildItem -LiteralPath $Evid -File -Filter 'status-*.log' -ErrorAction SilentlyContinue | ForEach-Object { if ($_.BaseName -match '^status-(\d+)$') { [int]$Matches[1] } })
        $sel = Select-NACaptures -Exe $ExeBase -Pids $runPids -Since $since
        $summary.Add(''); $summary.Add("Captures from this run: " + $(if ($sel.Matched) { $sel.Matched.Count } else { 'none' }))
        foreach ($cd in $sel.Matched) {
            $cdst = Join-Path $dst ('captures\' + $cd.Name); New-Item -ItemType Directory -Force -Path $cdst | Out-Null
            Get-ChildItem -LiteralPath $cd.FullName -File | Where-Object { $_.Extension -ne '.raw' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $cdst }
            $summary.Add("  $($cd.Name): metadata copied; raw planes stay in $($cd.FullName)")
        }
    }
    $summary | Set-Content -LiteralPath (Join-Path $dst 'summary.txt') -Encoding UTF8
    $sr = Join-Path $PSScriptRoot 'summarize-run.ps1'
    if ($logs -and (Test-Path -LiteralPath $sr)) { try { & $sr -Results $dst -Exe $ExeBase | Out-Null } catch { Log "Run summary failed; full logs are saved: $($_.Exception.Message)" } }
    Log "Results saved to $dst"
    return $dst
}
function Invoke-Rollback {
    if (-not (Test-Path -LiteralPath $Manifest)) { Log 'No installation record found; nothing to remove.'; return }
    $m = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
    if ($m.restoredUtc) { Log 'Already removed.'; return }
    foreach ($f in $m.files) {
        $name = [IO.Path]::GetFileName($f.path)
        $full = [IO.Path]::GetFullPath($f.path)
        # Our own files sit beside the exe; a component (another lane's file, e.g. a material pack) sits at its recorded place below it.
        $mine = if ($f.component) { $full.StartsWith($GameDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } else { $name -in $Owned -and $full -eq (Join-Path $GameDir $name) }
        if (-not $mine) { throw 'The installation record does not match this game folder. Nothing removed.' }
        if ($name -eq 'NeuralAliasHost.ini') { continue } # menu settings are saved into it: no hash guard
        if ((Test-Path -LiteralPath $f.path) -and (Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) { throw "Changed since install, left untouched: $($f.path)" }
    }
    foreach ($f in $m.files) {
        if (-not (Test-Path -LiteralPath $f.path)) { continue }
        if ([IO.Path]::GetFileName($f.path) -eq 'NeuralAliasHost.ini') { Copy-Item -LiteralPath $f.path -Destination (Join-Path $Evid 'NeuralAliasHost-saved.ini') -Force }
        Remove-Item -LiteralPath $f.path
    }
    $now = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
    $diff = Compare-Object -ReferenceObject @($m.folderSnapshot) -DifferenceObject $now
    if ($diff) { Log ("WARNING: the game folder differs from before the install: " + (($diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; ')) }
    else { Log 'Game folder matches its pre-install snapshot exactly.' }
    $m | Add-Member -NotePropertyName restoredUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $m | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Manifest
    Log "Removed: $Title is back to its original files."
}
$installStarted = $false
try {
    if ($Action -eq 'Results') { $r = Save-Results 'on request'; Write-Host "`nResults saved to $r" -ForegroundColor Green; exit 0 }
    $GameDir = (Resolve-Path -LiteralPath $GameDir).ProviderPath
    $gameExe = Join-Path $GameDir $Exe
    if (-not (Test-Path -LiteralPath $gameExe)) { throw "Game not found at $gameExe. Nothing changed." }
    if (Get-Process -Name (@($ExeBase) + $AlsoClose) -ErrorAction SilentlyContinue) { throw "$Title is running. Save and exit first." }
    if ($Action -eq 'Rollback') { [void](Save-Results 'before uninstall'); Invoke-Rollback; exit 0 }

    Set-Content -LiteralPath $Log -Value "NeuralAlias install for $Title, $(Get-Date -Format o)"
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
    $commit = [string]$package.source_commit
    if ($commit -notmatch '^[0-9a-f]{7,40}$') { throw 'The package does not name its build. Nothing changed.' }
    foreach ($payload in $package.payload) {
        if ($payload.name -ne [IO.Path]::GetFileName($payload.name) -or (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $payload.name)).Hash -ne $payload.sha256) { throw "Package mismatch: $($payload.name). Nothing changed." }
    }
    $dll = Join-Path $PSScriptRoot 'NeuralAliasDXGI.dll'
    if ('NeuralAliasDXGI.dll' -notin @($package.payload.name)) { throw 'Incomplete package: NeuralAliasDXGI.dll. Nothing changed.' }
    $gameBits = Bits (Get-PEMachine $gameExe); $proxyBits = Bits (Get-PEMachine $dll)
    Log "Build $commit. Game is $gameBits, program is $proxyBits."
    if ($gameBits -ne $proxyBits) { throw "The game is $gameBits and this package's program is $proxyBits; the game would never load it. Nothing changed." }
    # DLSS exists only as a 64-bit runtime. A 64-bit game loads it itself; a 32-bit game needs the
    # 64-bit companion program beside it, which loads it instead.
    $sdkDll = Join-Path $PSScriptRoot 'nvngx_dlss.dll'
    $companion = Join-Path $PSScriptRoot 'NeuralAliasDlaa64.exe'
    $sdk64 = (Test-Path -LiteralPath $sdkDll) -and ((Bits (Get-PEMachine $sdkDll)) -eq '64-bit')
    $haveCompanion = $gameBits -eq '32-bit' -and (Test-Path -LiteralPath $companion) -and ((Bits (Get-PEMachine $companion)) -eq '64-bit')
    $haveDlss = $sdk64 -and ($gameBits -eq '64-bit' -or $haveCompanion)
    if ($haveCompanion -and $haveDlss) { Log 'The game is 32-bit: DLAA runs in the 64-bit companion program installed beside it.' }
    if (-not $haveDlss) { Log 'This package cannot run DLAA in this game (no 64-bit DLSS runtime, or no companion for a 32-bit game): hooks and the Home menu run, DLAA stays off and the log says why.' }
    $components = @($package.components | Where-Object { $_ })
    foreach ($c in $components) {
        $rel = [string]$c.dest
        if (-not $rel -or [IO.Path]::IsPathRooted($rel) -or $rel -match '(^|[\\/])\.\.([\\/]|$)') { throw "Component $($c.name) has no safe destination. Nothing changed." }
        $src = Join-Path $PSScriptRoot $c.name
        if ($c.name -ne [IO.Path]::GetFileName($c.name) -or -not (Test-Path -LiteralPath $src) -or (Get-FileHash -LiteralPath $src).Hash -ne $c.sha256) { throw "Component mismatch: $($c.name). Nothing changed." }
        if (-not (Test-Path -LiteralPath (Split-Path -Parent (Join-Path $GameDir $rel)))) { throw "Component $($c.name): the folder for $rel does not exist in the game. Nothing changed." }
    }

    $keep = @{ sharpness = '100'; preset = 'M'; mip_bias = '-1.0'; flow_loosen = '50' }
    if (Test-Path -LiteralPath $Manifest) {
        $old = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
        if (-not $old.restoredUtc) { [void](Save-Results 'previous build, before update'); Log 'A previous build is installed: removing it first (its logs are kept).'; Invoke-Rollback }
        $saved = Join-Path $Evid 'NeuralAliasHost-saved.ini'
        if (Test-Path -LiteralPath $saved) {
            $j2 = [regex]::Match((Get-Content -LiteralPath $saved -Raw), '(?ms)^\[j2\](.*?)(^\[|\z)').Groups[1].Value
            foreach ($k in @($keep.Keys)) { $mm = [regex]::Match($j2, "(?m)^$k=([^\r\n]+)"); if ($mm.Success) { $keep[$k] = $mm.Groups[1].Value.Trim() } }
        }
        Move-Item -LiteralPath $Evid -Destination ($Evid + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    }
    New-Item -ItemType Directory -Force -Path $Evid | Out-Null
    foreach ($n in $Owned + 'd3d11.dll' + @($components | ForEach-Object { [string]$_.dest })) { if (Test-Path -LiteralPath (Join-Path $GameDir $n)) { throw "$n already exists in the game folder. This script never overwrites game files. Nothing changed." } }

    $ini = Join-Path $Evid 'NeuralAliasHost.ini'
    @"
[host]
build_commit=$commit
executable=$Exe
graphics_api=$(if ($autoApi) { 'auto' })
start_after_factory=$(if ($StartAfterFactory) { 1 } else { 0 })
startup_log=$Evid\startup.log
status_log=$Evid\status.log
enabled=1
tag=$(if ($autoApi) { "NeuralAlias ($Title)" } else { "Native DX11 ($Title)" })
[dx11on12]
enabled=0
[native11]
enabled=$(if ($autoApi) { 0 } else { 1 })
[overlay]
enabled=1
[settings]
scope=$Label
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
enabled=$(if ($autoApi) { 1 } else { 0 })
[observe]
profile_hooks=0
usage_log=1
producers=$(if ($autoApi) { 1 } else { 0 })
early_install=$(if ($autoApi) { 1 } else { 0 })
[discovery]
enabled=$(if ($autoApi) { 1 } else { 0 })
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
flow_loosen=$($keep['flow_loosen'])
mip_bias=$($keep['mip_bias'])
mip_bias_live=1
sharpness=$($keep['sharpness'])
preset=$($keep['preset'])
route=auto
capture_frames=30
capture_auto=0
capture_cooldown_seconds=20
[diagnostics]
camera_inputs=1
[layer2]
enabled=0
"@ | Set-Content -LiteralPath $ini -Encoding ascii
    $sources = [ordered]@{ 'dxgi.dll' = $dll; 'NeuralAliasHost.ini' = $ini }
    if ($haveDlss) { $sources['nvngx_dlss.dll'] = $sdkDll }
    if ($haveDlss -and $haveCompanion) { $sources['NeuralAliasDlaa64.exe'] = $companion }
    $lic = Join-Path $PSScriptRoot 'AMD-OpticalFlow-LICENSE.txt'
    if (Test-Path -LiteralPath $lic) { $sources['NeuralAlias-AMD-OpticalFlow-LICENSE.txt'] = $lic }
    $files = @(foreach ($n in $sources.Keys) { @{ path = (Join-Path $GameDir $n); source = $sources[$n]; deployedHash = (Get-FileHash -LiteralPath $sources[$n]).Hash } })
    $files += @(foreach ($c in $components) { @{ path = (Join-Path $GameDir ([string]$c.dest)); source = (Join-Path $PSScriptRoot $c.name); deployedHash = [string]$c.sha256; component = $true } })
    $snapshot = @(Get-ChildItem -LiteralPath $GameDir -Force | ForEach-Object { "{0}|{1}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { $_.Length }) })
    @{ createdUtc = [DateTime]::UtcNow.ToString('o'); sourceCommit = $commit; gameBits = $gameBits; proxyBits = $proxyBits; dlssRuntime = $haveDlss
       folderSnapshot = $snapshot; gameExeHash = (Get-FileHash -LiteralPath $gameExe).Hash; files = $files } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Manifest
    $installStarted = $true
    foreach ($f in $files) {
        [IO.File]::Copy($f.source, $f.path, $false)
        if ((Get-FileHash -LiteralPath $f.path).Hash -ne $f.deployedHash) { throw 'Install hash mismatch' }
    }
    $installStarted = $false
    Log "READY: build $commit installed for $Title."
    Write-Host "`nREADY. Build $commit is installed. Start $Title as usual; press Home in the game for the menu." -ForegroundColor Green
    if (-not $haveDlss) { Write-Host 'This package has no DLAA: it checks that the program loads, the Home menu shows and the game stays stable.' -ForegroundColor Yellow }
    foreach ($c in $components) { Write-Host "Also installed: $($c.dest)" -ForegroundColor Green }
} catch {
    if ($installStarted) { try { Invoke-Rollback } catch { Log "Partial installation record kept for recovery: $_" } }
    Log "STOPPED: $($_.Exception.Message)"
    Write-Host "`nSTOPPED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
