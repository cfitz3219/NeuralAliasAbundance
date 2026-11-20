$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/referee-summary.ps1')

function Frame([int]$ordinal, [bool]$early) {
    $laterLabel = if ($early) { 'picture' } else { 'SELECTED picture' }
    $earlyLabel = if ($early) { 'SELECTED other' } else { 'other' }
    $later = "[$laterLabel fmt=9 draws=207 multi=6 first=2105 last=2400 first_read=2312 read_after=2401 last_read=2407 float=1 same_size=1]"
    $earlier = "[$earlyLabel fmt=9 draws=67 multi=64 first=1347 last=2390 first_read=2104 read_after=2391 last_read=2399 float=1 same_size=1]"
    $candidates = if ($early) { "$later $earlier" } else { "$earlier $later" }
    return "event src=code native11_scene frame=$ordinal rule=after-layers candidates: $candidates"
}

function Check([string]$name, [string[]]$lines, [bool]$expected) {
    $result = @(Get-RefereeSummary $lines | Where-Object { $_ -like '*DX11 layer order*' })
    if (($result.Count -gt 0) -ne $expected) { throw "Unexpected result for $name" }
}

$alternating = @(1..8 | ForEach-Object { Frame $_ ($_ % 2 -eq 0) })
Check 'alternating with reversed candidate order' $alternating $true
Check 'fixed selection' @(1..12 | ForEach-Object { Frame $_ $false }) $false
Check 'short burst' $alternating[0..5] $false
Check 'missing ordinal' ($alternating[0..3] + $alternating[5..7] + (Frame 9 $false)) $false
Check 'resources used concurrently' ($alternating -replace 'read_after=2391', 'read_after=2405') $false
Check 'additional plausible target' ($alternating | ForEach-Object { $_ + ' [other fmt=9 draws=8 multi=1 first=10 last=30 first_read=20 read_after=31 last_read=35 float=1 same_size=1]' }) $false
Check 'other extent' ($alternating -replace 'last_read=2399 float=1 same_size=1','last_read=2399 float=1 same_size=0') $false
Check 'scene rule changes' ($alternating[0..3] + ($alternating[4..7] -replace 'rule=after-layers ', 'rule=after-layers-second-depth ')) $false
Write-Output 'Referee layer-order summary passed all eight cases.'
$coverage = @(Get-RefereeSummary @('event referee_coverage dx12_scene=90 dx12_submitted=0 dx11_presents=25 dx11_submitted=22'))
if (($coverage -like '*not running*') -or -not ($coverage -like '*DX12 submitted DLAA on 0 of 90*') -or
    -not ($coverage -like '*DX11 submitted DLAA on 22 of 25*')) { throw 'Lost coverage before first qualified image' }
$active = @(Get-RefereeSummary @('event native11_lit_role frame=200 role_deliveries=64 scene_stream=4'))
if (-not ($active -like '*at least 64 submissions*') -or ($active -like '*not running*')) { throw 'Missing observed lighting-route delivery' }
$unproven = @(Get-RefereeSummary @('event native11_lit_role frame=201 reader=adaptive_candidate_mid_scene'))
if ($unproven -like '*used the screen-verified lighting route*') { throw 'Reported an unverified lighting route as active' }

$trial = @(
    'event native11_route_trial next_frame=301 phase=baseline region=0 stops=0.000/0.000/0.000 why='
    'event native11_route_trial next_frame=603 phase=kept region=36 stops=0.246/0.000/0.246 why=measured improvement'
)
$outcome = @(Get-RefereeSummary $trial)
if (-not ($outcome -like '*Replacement kept. measured improvement*') -or -not ($outcome -like '*0.246/0.000/0.246*')) {
    throw 'Missing pixel comparison verdict'
}
$rollback = @(Get-RefereeSummary ($trial + 'event native11_route_trial next_frame=647 phase=rejected region=36 stops=0.246/0.000/0.246 why=lost reader'))
if (-not ($rollback -like '*Ordinary route restored. lost reader*') -or ($rollback -like '*Replacement kept*')) {
    throw 'Missing latest fallback verdict'
}
$sequence = @(Get-RefereeSummary @(
    'event referee_recovery frame=100 first_stage=input region=36 tool=reduce_bloom phase=restored ruled_out=1 stops=0.24/0.25/0.00 why=the replacement did not improve the presented picture'
    'event referee_recovery frame=200 first_stage=input region=36 tool=current_frame_ambient_shading phase=kept ruled_out=1 stops=0.24/0.01/0.24 why=the replacement reduced flicker'
    'event referee_recovery frame=300 first_stage=composition region=20 tool=observe phase=no_matching_tool ruled_out=0 stops=0/0/0 why=no submitted control matches the measured stage'
))
if (-not ($sequence -like '*after trying reduced bloom*') -or -not ($sequence -like '*Kept current-frame ambient shading*') -or
    -not ($sequence -like '*no submitted control matches*')) { throw 'Missing stage-specific recovery sequence' }

$regional = @(Get-RefereeSummary @(
    'event referee_composite frame=98 path=native11 input_xy=81,32 control_xy=240,46 tile_size=32 input_mask=1 output_mask=28'
    'event native11_route_trial next_frame=201 phase=kept region=11 stops=0.000/0.000/0.000 regional=1 why=the original bypass returned on recheck'
))
if (-not ($regional -like '*81,32*240,46*') -or -not ($regional -like '*Replacement kept*original bypass*') -or
    ($regional -like '*brightness change in stops*')) { throw 'Regional replacement evidence was lost in the results' }

$dx12 = @(Get-RefereeSummary @(
    'event dx12_route_trial next_frame=202 phase=kept region=11 stops=0.000/0.000/0.000 regional=1 why=the original bypass returned on recheck'
))
if (-not ($dx12 -like '*Replacement kept*original bypass*')) { throw 'DX12 placement trial was lost in the results' }
