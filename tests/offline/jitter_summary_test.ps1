$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/jitter-summary.ps1')

# Lines in the shape of the native11_jitter event (host_dx12/native11/provider.cpp).
function Row([int]$frame, [int]$applied, [int]$shifted, [int]$scene, [int]$unshifted, [int]$untested, [string]$told = '-0.1866,0.4079') {
    return "event t=1 frame=0 thread=1 src=code native11_jitter frame=$frame route=viewport told_px=$told viewport_shift_px=$told applied=$applied " +
           "shifted_draws=$shifted unshifted_colour_draws=$unshifted scene_draws=$scene held=0 scene=1280x720 depth=1280x720 shifted_vertices=10 " +
           "still_meshes{draws=$untested untested=$untested vertices=0} aligned_shader_depth_draws=0 referee_still_meshes{asked=0 moved_draws=0} " +
           "variant=jitter/scene-overlays overlays_moved=5 still_by{deferred=0 other_target=0 own_viewport=0 rule_off=0 stationary_source=0 tested_or_other=0}"
}
function Check([string]$name, [string[]]$lines, [string]$expect) {
    $text = (Get-JitterSummary $lines) -join "`n"
    if ($text -notlike "*$expect*") { throw "Unexpected result for ${name}:`n$text" }
}

Check 'no lines' @('status line') 'no native11_jitter lines'
Check 'clean' @(1..4 | ForEach-Object { Row $_ 1 90 100 0 0 }) 'nothing left unmoved'
Check 'clean share' @(1..4 | ForEach-Object { Row $_ 1 90 100 0 0 }) 'Scene draws moved: 360 of 400 (90.0 %)'
Check 'untested still meshes' @((Row 1 1 90 100 0 0), (Row 2 1 80 100 20 20)) 'untested still meshes on 1 frame(s)'
Check 'unmoved colour draws' @((Row 1 1 80 100 20 0)) 'colour draws left unmoved on 1 frame(s)'
Check 'told, not applied' @((Row 1 0 0 100 0 0)) '1 frame(s) told DLAA a jitter the scene never got'
Check 'no jitter told' @((Row 1 0 0 100 0 0 '0.0000,0.0000')) 'told a jitter but not applied on 0'
Write-Output 'jitter summary: all passed'
