[CmdletBinding()]
param(
    [ValidateSet('Install','Rollback','Status','Validate')][string]$Action = 'Install',
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\PBZ',
    [string]$PackageRoot,
    [string]$StateRoot,
    [string]$UserRoot = (Join-Path $env:LOCALAPPDATA 'NeuralAlias\PBZ'),
    [string]$ProfileTool
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $PackageRoot) { $PackageRoot = $PSScriptRoot }
if (-not $StateRoot) { $StateRoot = Join-Path $PackageRoot 'state' }
if (-not $ProfileTool) { $ProfileTool = Join-Path $PackageRoot 'NAProfile.exe' }
$GameRoot = [IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$StateRoot = [IO.Path]::GetFullPath($StateRoot)
$UserRoot = [IO.Path]::GetFullPath($UserRoot)
$manifestPath = Join-Path $StateRoot 'deployment.json'
$profileBackup = Join-Path $StateRoot 'FG-profile-before.ini'
$helper = $ProfileTool
function Hash($p) { if (Test-Path -LiteralPath $p -PathType Leaf) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }
function Save-Manifest($m) {
    $m | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$manifestPath.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$manifestPath.tmp" -Destination $manifestPath -Force
}
function Profile([string]$verb) {
    & $helper $verb 'PBZ.exe' $profileBackup 3
    if ($LASTEXITCODE -ne 0) { throw "NVIDIA profile $verb failed ($LASTEXITCODE)." }
}
function Assert-Closed {
    if (Get-Process -Name PBZ -ErrorAction SilentlyContinue) { throw 'Close Phantom Blade Zero before installing or rolling back.' }
}
function Restore($m) {
    if ($m.game_root -ne $GameRoot -or $m.user_root -ne $UserRoot) { throw 'Rollback paths differ from the recorded installation.' }
    # Check every original before restoring any file. Mutable menu settings are archived.
    foreach ($e in $m.files) {
        if ($e.before_hash -and (Hash $e.backup) -ne $e.before_hash) { throw "Backup mismatch: $($e.backup)" }
        $now = Hash $e.destination
        if ($e.touched -and -not $e.mutable -and $now -and $now -ne $e.after_hash -and $now -ne $e.before_hash) {
            throw "File changed after installation; retained for inspection: $($e.destination)"
        }
    }
    foreach ($e in $m.files) {
        if (-not $e.touched) { continue }
        if ($e.mutable -and (Test-Path -LiteralPath $e.destination)) {
            Copy-Item -LiteralPath $e.destination -Destination (Join-Path $StateRoot 'last-user-settings.ini') -Force
        }
        if ($e.before_hash) {
            Copy-Item -LiteralPath $e.backup -Destination $e.destination -Force
            if ((Hash $e.destination) -ne $e.before_hash) { throw "Restore verification failed: $($e.destination)" }
        } elseif (Test-Path -LiteralPath $e.destination) {
            Remove-Item -LiteralPath $e.destination -Force
        }
    }
    $m.state = 'restored'; Save-Manifest $m
    # The driver profile comes back AFTER the files, and its failure never strands
    # our files in the game folder (a driver update can move or rebuild the profile).
    $profileRestored = $true
    if ($m.profile_attempted -and (Test-Path -LiteralPath $profileBackup)) {
        try { Profile 'restore' } catch { $profileRestored = $false; Write-Warning "Game folder restored, but the NVIDIA FG preset could not be put back: $_. Run Rollback again later, or reset the Frame Generation override for PBZ.exe in the NVIDIA App." }
    }
    if ($profileRestored) { Write-Host 'Game folder restored (NeuralAlias files removed; no game file was ever changed). Menu preferences and FG driver settings restored.' }
}
if ($Action -eq 'Status') {
    if (-not (Test-Path -LiteralPath $manifestPath)) { Write-Host 'Not installed by this package.'; return }
    $m = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    Write-Host "State: $($m.state); source: $($m.source_commit)"
    $m.files | ForEach-Object { [PSCustomObject]@{File=$_.destination; MatchesInstalled=((Hash $_.destination) -eq $_.after_hash)} }
    return
}
Assert-Closed
if ($Action -eq 'Rollback') {
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'No deployment manifest exists.' }
    Restore (Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json)
    return
}
$package = Get-Content -Raw -LiteralPath (Join-Path $PackageRoot 'package.json') | ConvertFrom-Json
foreach ($f in $package.payload) {
    if ($f.name -notin @('NeuralAliasDXGI.dll','NAProfile.exe','nvngx_dlssnr.dll','NVIDIA-NR-LICENSE.txt','deploy-pbz.ps1','pbz-package-run.ps1','README.md','ENGINEERING-NOTES.md','repair-from.txt')) {
        throw "Not a package payload file: $($f.name). Logs and deployment state must not enter the payload manifest."
    }
    if ((Hash (Join-Path $PackageRoot $f.name)) -ne $f.sha256) { throw "Package file mismatch: $($f.name)" }
}
foreach ($f in @('nvngx_dlssnr.dll')) {
    $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $f)
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') { throw "NVIDIA signature invalid: $f" }
}
if (-not (Test-Path -LiteralPath (Join-Path $GameRoot 'platform\steam\PBZ.exe'))) { throw 'Phantom Blade Zero executable not found.' }
if ($Action -eq 'Validate') { Write-Host 'Replacement package validated; installation unchanged.'; return }
if (Test-Path -LiteralPath $manifestPath) {
    $prior = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($prior.state -ne 'restored') { throw 'An installation already exists. Use Status or Rollback first.' }
    $archive = Join-Path $StateRoot ('previous-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $archive | Out-Null
    Get-ChildItem -LiteralPath $StateRoot -File | ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination $archive }
}
& $helper status PBZ.exe
if ($LASTEXITCODE -ne 0) { throw 'Phantom Blade Zero NVIDIA profile was not found.' }
New-Item -ItemType Directory -Force -Path $StateRoot,$UserRoot | Out-Null
$ini = @"
[settings]
scope=PBZ
[host]
enabled=1
tag=PHANTOM BLADE ZERO
status_log=$UserRoot\status.log
startup_log=$UserRoot\startup.log
menu_diagnostics=1
[route_a]
enabled=1
[observe]
producers=0
sync_loader_hook=0
detail=0
log=$UserRoot\observe.log
[overlay]
enabled=1
[present]
proxy=0
[dlss]
enabled=0
mode=4
follow_game_resolution=1
preset=13
[nr]
enabled=0
model=0
[fg]
enabled=0
profile_override=1
preset=3
[j2]
enabled=0
[fsr]
enabled=0
[xess]
enabled=0
"@
$iniSource = Join-Path $StateRoot 'defaults.ini'
$ini | Set-Content -LiteralPath $iniSource -Encoding Unicode
$plans = @()
foreach ($dir in @('platform\steam')) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameRoot "$dir\PBZ.exe"))) { continue }
    foreach ($pair in @(@('NeuralAliasDXGI.dll','dxgi.dll'),@('nvngx_dlssnr.dll','nvngx_dlssnr.dll'))) {
        $plans += @{source=(Join-Path $PackageRoot $pair[0]); destination=(Join-Path $GameRoot "$dir\$($pair[1])"); mutable=$false}
    }
    $plans += @{source=$iniSource; destination=(Join-Path $GameRoot "$dir\NeuralAliasHost.ini"); mutable=$false}
}
# The game's own nvngx_dlss / nvngx_dlssd / nvngx_dlssg (310.9.1, already able to run
# presets L/M; FG preset C is a driver-profile request) are NEVER replaced. Established
# rule (Anno, Sniper Elite, TLOU): the installer only ADDS files to the game folder.
$dlssDir = 'Engine\Plugins\Runtime\Nvidia\DLSS\Binaries\ThirdParty\Win64'
# NGX can use the plugin search directory rather than the executable directory.
$plans += @{source=(Join-Path $PackageRoot 'nvngx_dlssnr.dll'); destination=(Join-Path $GameRoot "$dlssDir\nvngx_dlssnr.dll"); mutable=$false}
$plans += @{source=$iniSource; destination=(Join-Path $UserRoot 'NeuralAliasSL.ini'); mutable=$true}
$plans += @{source=$profileBackup; destination=(Join-Path $UserRoot 'FG-profile-before.ini'); mutable=$false}
foreach ($p in $plans) {
    if ($p.destination.StartsWith($GameRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $p.destination)) {
        throw "Refusing: $($p.destination) already exists. This installer only adds files to the game folder and never replaces one. Nothing changed."
    }
}
$m = [PSCustomObject]@{state='preparing'; source_commit=$package.source_commit; game_root=$GameRoot; user_root=$UserRoot; profile_attempted=$false; files=@()}
foreach ($p in $plans) {
    $original = Hash $p.destination
    $backup = Join-Path $StateRoot ("original-{0}.bin" -f $m.files.Count)
    if ($original) {
        Copy-Item -LiteralPath $p.destination -Destination $backup
        if ((Hash $backup) -ne $original) { throw "Backup verification failed: $($p.destination)" }
    }
    $m.files += [PSCustomObject]@{destination=$p.destination; source=$p.source; backup=$backup; before_hash=$original; after_hash=$null; mutable=$p.mutable; touched=$false}
}
Save-Manifest $m
try {
    $m.profile_attempted=$true; Save-Manifest $m
    Profile 'apply'
    foreach ($e in $m.files) {
        $e.after_hash=Hash $e.source
        if (-not $e.after_hash) { throw "Source missing: $($e.source)" }
        $e.touched=$true; Save-Manifest $m
        $stage = "$($e.destination).na-stage"
        if (Test-Path -LiteralPath $stage) { throw "Unfinished staging file exists: $stage" }
        try {
            Copy-Item -LiteralPath $e.source -Destination $stage
            if ((Hash $stage) -ne $e.after_hash) { throw "Staged file verification failed: $stage" }
            Move-Item -LiteralPath $stage -Destination $e.destination -Force
        } finally {
            if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Force }
        }
        if ((Hash $e.destination) -ne $e.after_hash) { throw "Installed file verification failed: $($e.destination)" }
    }
    $m.state='installed'; Save-Manifest $m
    Write-Host 'Installed. Launch normally. Keep DLSS Ultra Performance in the game; Home opens NeuralAlias. Turn Neural Rendering on there when ready.'
    Write-Host 'Native game Frame Generation remains in control; its driver preset request is C. No game was launched.'
} catch {
    $failure=$_
    try { Restore $m } catch { Write-Warning "Automatic rollback needs attention: $_. Run Rollback again after resolving it." }
    throw $failure
}
