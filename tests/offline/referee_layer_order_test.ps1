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
$active = @(Get-RefereeSummary @('event native11_lit_role frame=200 role_deliveries=64 scene_stream=4'))
if (-not ($active -like '*at least 64 submissions*') -or ($active -like '*not running*')) { throw 'Missing observed lighting-route delivery' }
$unproven = @(Get-RefereeSummary @('event native11_lit_role frame=201 reader=adaptive_candidate_mid_scene'))
if ($unproven -like '*used the screen-verified lighting route*') { throw 'Reported an unverified lighting route as active' }
