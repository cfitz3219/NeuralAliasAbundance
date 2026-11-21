# What the referee found during a run, in plain lines, read from a status log's `referee`,
# `referee_state`, `referee_choice` and `referee_coverage` event lines. Dot-source it and call
# Get-RefereeSummary with the log's lines. No game is named here; any package's results script
# can use it (deploy-native11.ps1 does, when this file sits beside it).
# These are completed-frame layer reports, already written by the native provider.
# A frame may compare two eligible targets without either having screen proof.
function Get-Native11LayerOrderFinding([string[]] $Lines) {
    $header = [regex]'\bnative11_scene frame=(\d+) rule=(after-layers(?:-second-depth)?) '
    $candidate = [regex]'\[(?<role>(?:SELECTED )?(?:picture|other)) fmt=(?<fmt>\d+) draws=\d+ multi=\d+ first=\d+ last=(?<write>\d+) first_read=\d+ read_after=(?<read>\d+) last_read=\d+ float=1 same_size=1\]'
    $previousFrame = -2L
    $previousRole = ''
    $previousGroup = ''
    $length = 0
    $longest = 0
    $last = 0L
    foreach ($line in $Lines) {
        if ($line.IndexOf('native11_scene frame=', [StringComparison]::Ordinal) -lt 0) { continue }
        $record = $header.Match($line)
        if (!$record.Success) { $length = 0; continue }
        $frame = [long]$record.Groups[1].Value
        $pair = $candidate.Matches($line)
        if ($pair.Count -ne 2) { $length = 0; continue }
        $picture, $other = $pair[0], $pair[1]
        if ($picture.Groups['role'].Value.EndsWith('other')) { $picture, $other = $other, $picture }
        if (!$picture.Groups['role'].Value.EndsWith('picture') -or !$other.Groups['role'].Value.EndsWith('other') -or
            $other.Groups['fmt'].Value -ne $picture.Groups['fmt'].Value) { $length = 0; continue }

        $earlierWrite = [long]$other.Groups['write'].Value
        $earlierRead = [long]$other.Groups['read'].Value
        $laterWrite = [long]$picture.Groups['write'].Value
        $laterRead = [long]$picture.Groups['read'].Value
        if (!$earlierWrite -or $earlierWrite -ge $earlierRead -or $earlierRead -ge $laterWrite -or
            $laterWrite -ge $laterRead) { $length = 0; continue }
        if ($picture.Groups['role'].Value -eq 'SELECTED picture') { $role = 'later' }
        elseif ($other.Groups['role'].Value -eq 'SELECTED other') { $role = 'earlier' }
        else { $length = 0; continue }

        $group = $record.Groups[2].Value + '/' + $picture.Groups['fmt'].Value
        if ($frame -eq $previousFrame + 1 -and $group -eq $previousGroup -and $role -ne $previousRole) { ++$length }
        else { $length = 1 }
        $previousFrame, $previousGroup, $previousRole = $frame, $group, $role
        if ($length -gt $longest) { $longest, $last = $length, $frame }
    }
    if ($longest -ge 8) {
        $first = $last - $longest + 1
        return "  DX11 layer order: selection alternated between earlier and later scene-color candidates for $longest consecutive recorded frames ($first-$last). Compare both paths at the screen before attributing flicker to either."
    }
}

function Get-RefereeSummary([string[]] $Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $layerOrder = Get-Native11LayerOrderFinding $Lines
    $windows = @($Lines | Where-Object { $_ -match '(^|\s)referee\s+samples=\d+' })
    $states = @($Lines | Where-Object { $_ -match '(^|\s)referee_state\s' })
    $coverage = $Lines | Where-Object { $_ -match '(^|\s)referee_coverage\s' } | Select-Object -Last 1
    $roles = @($Lines | Where-Object { $_ -match '\bnative11_lit_role frame=\d+ role_deliveries=(\d+) scene_stream=\d+' })
    $traces = @($Lines | Where-Object { $_ -match '\breferee_trace\s+frames=\d+,\d+ first_stage=(input|reconstruction|composition) ' })
    $trials = @($Lines | Where-Object { $_ -match '\b(?:native11|dx12)_route_trial next_frame=\d+ phase=' })
    $recovery = @($Lines | Where-Object { $_ -match '\breferee_recovery frame=\d+ first_stage=' })
    $localRaster = $Lines | Where-Object { $_ -match '\breferee_mixed_raster frame=' } | Select-Object -Last 1
    $jitterAction = $Lines | Where-Object { $_ -match '\breferee_jitter_trial frame=' } | Select-Object -Last 1
    $jitterGeometry = $Lines | Where-Object { $_ -match '\breferee_jitter_geometry frame=' } | Select-Object -Last 1
    $composite = $Lines | Where-Object { $_ -match '\breferee_composite frame=\d+ path=' } | Select-Object -Last 1
    $parity = @($Lines | Where-Object { $_ -match '\breferee_parity_swap frames=\d+,\d+ ' })
    $kept = @($Lines | Where-Object { $_ -match '\breferee_roles frame=\d+ kept=1' })
    if (-not $windows -and -not $states -and -not $roles -and -not $traces -and -not $trials -and -not $coverage -and -not $recovery -and -not $composite -and -not $jitterGeometry -and -not $localRaster -and -not $jitterAction -and -not $parity -and -not $kept) {
        $enabled = $Lines | Where-Object { $_ -match '\b(?:setting |settings_all\s+.*\b)referee_enabled=1\b' } | Select-Object -First 1
        $startup = $Lines | Where-Object { $_ -match '\breferee_start automatic_dlaa=1 gpu_checks=([01]) enabled=([01])' } | Select-Object -Last 1
        if ($startup -match 'gpu_checks=1 enabled=1') { $out.Add('Referee started automatically with DLAA. The log has no screen or delivery checks.') }
        elseif ($startup -match 'gpu_checks=0') { $out.Add('This build lacks the GPU checks required for Referee. No screen checks ran.') }
        elseif ($enabled) { $out.Add('Referee was enabled during this run. The log has no screen or delivery checks.') }
        elseif ($Lines -match '\breferee_enabled=0\b') { $out.Add('Referee was recorded off. No screen or delivery checks were logged.') }
        else { $out.Add('The log has no Referee screen or delivery checks.') }
        if ($layerOrder) { $out.Add($layerOrder) }
        return $out
    }
    $out.Add('Referee screen and delivery checks')
    if ($coverage) {
        foreach ($api in 'dx11','dx12') {
            $seen = if ($api -eq 'dx11') { 'dx11_presents' } else { 'dx12_scene' }
            if ($coverage -match "\b$seen=(\d+)" -and ($eligible = [long]$Matches[1]) -gt 0 -and
                $coverage -match "\b${api}_submitted=(\d+)") {
                $out.Add("  $($api.ToUpperInvariant()) submitted DLAA on $($Matches[1]) of $eligible eligible frames at the last checkpoint.")
            }
        }
    }
    if ($traces) {
        $stages = $traces | ForEach-Object { if ($_ -match '\bfirst_stage=(\w+)') { $Matches[1] } } | Group-Object
        $out.Add('  Local flicker first measured at ' + (($stages | ForEach-Object { "$($_.Name) ($($_.Count))" }) -join ', '))
        $out.Add('  Last matched pair: ' + ($traces[-1] -replace '^.*?referee_trace\s+', ''))
    }
    if ($parity) {
        $null = $parity[-1] -match 'brighter=(\w+)'
        $out.Add("  Parity flicker recognized $($parity.Count) time(s): the screen flipped brightness every other frame ($($Matches[1]) frames brighter), opposite to the DLAA input. Cause: two scene buffers swap jobs each frame.")
    }
    if ($kept) {
        $null = $kept[-1] -match 'referee_roles frame=(\d+)'
        $keptFrame = [long]$Matches[1]
        $after = @($traces | Where-Object { $_ -match 'referee_trace\s+frames=\d+,(\d+) ' -and [long]$Matches[1] -gt $keptFrame })
        $out.Add("  Role-following route kept from frame $keptFrame ($($kept.Count) keep(s)); no timeout can restore the flickering route. Traced flashes after the last keep: $($after.Count).")
    }
    if ($roles) {
        $null = $roles[-1] -match 'role_deliveries=(\d+) scene_stream=(\d+)'
        $out.Add("  Rotating lighting buffers: DLAA used the screen-verified lighting route. Last checkpoint for scene $($Matches[2]): at least $($Matches[1]) submissions.")
    }
    if ($composite -match 'input_xy=(\d+,\d+) control_xy=(\d+,\d+) tile_size=(\d+)') {
        $out.Add("  Repeated regional difference. A $($Matches[3])-pixel tile at ($($Matches[1])) matched the input. A control at ($($Matches[2])) matched the reconstructed picture.")
    }
    if ($trials -and $trials[-1] -match '\bphase=(\w+) region=(\d+) stops=([^ ]+)(?: regional=(\d+))?(?: verified_reduction=([-\d.]+))? why=(.*)$') {
        $regional = $Matches[4] -eq '1'
        $primaryRegion = $Matches[2]
        $why = $Matches[6]
        $stops = $Matches[3]
        $trialState = switch ($Matches[1]) {
            'kept' { 'Replacement kept' }
            'rejected' { 'Ordinary route restored' }
            default { 'Comparison in progress at the last recorded frame' }
        }
        $out.Add("  $trialState. $why")
        if ($regional) { $out.Add('  The comparison follows the same disputed tiles and clear controls through the ordinary route, replacement, and ordinary retest.') }
        else { $out.Add("  Brightness change in stops (ordinary region $primaryRegion / largest during replacement / ordinary region retest): $stops.") }
        if ($Matches[1] -eq 'kept' -and $Matches[5] -and [double]$Matches[5] -ge 0) {
            $reduction = [double]$Matches[5]
            $goal = if ($reduction -ge .90) { 'met the 90% target' } else { 'below the 90% target' }
            $out.Add(('  Measured {0:P1} less brightness variation in region {1}, {2} for that region.' -f $reduction, $primaryRegion, $goal))
        }
    }
    $evidence = @($Lines | Where-Object { $_ -match '\breferee_evidence owner=\w+ frame=\d+ phase=\w+ now=rejected ' })
    if ($evidence) {
        $outcomes = $evidence | ForEach-Object { if ($_ -match '\boutcome=(\w+)') { $Matches[1] } } | Group-Object
        $labels = @{ harm = 'made the picture worse'; insufficient_evidence = 'not enough evidence'; unavailable = 'replacement could not run'; slower = 'cost too much frame time' }
        $out.Add('  Ended comparisons: ' + (($outcomes | ForEach-Object {
            $label = if ($labels.ContainsKey($_.Name)) { $labels[$_.Name] } else { $_.Name }
            "$label ($($_.Count))" }) -join ', ') + '.')
        if ($evidence[-1] -match 'owner=(\w+) frame=\d+ phase=(\w+) now=\w+ outcome=(\w+) receipts=(\d+) used=(\d+) decisive=(\w+) retries=(\d+) refused=(\S+)') {
            $gates = @{
                no_receipt = 'no screen check arrived'; other_experiment = 'the screen checks belonged to another comparison'
                stale = 'the screen checks were too old'; other_scope = 'the screen checks showed a different scene or route'
                not_comparable = 'the frame pairs could not be compared'; not_delivered = 'the replacement was not confirmed on screen'
                invalid = 'the measurements were not usable numbers'; view_changed = 'the view kept changing'
                region_unverified = 'the disputed area could not be verified'; symptom_absent = 'the symptom did not come back'
                no_motion = 'no motion measurement arrived'; none = 'nothing was refused'
            }
            $block = if ($gates.ContainsKey($Matches[6])) { $gates[$Matches[6]] } else { $Matches[6] }
            $phase = $Matches[2] -replace '_', ' '
            $again = if ($Matches[3] -eq 'harm') { 'It will not be tried again for this scene.' } else { 'It can be tried again.' }
            $out.Add("  Last ended comparison ($($Matches[1]), $phase phase): $($Matches[5]) of $($Matches[4]) screen checks were usable; main blocker: $block. $again")
            if ($evidence[-1] -match '\bframe_ms=([\d.]+)/([\d.]+)/([\d.]+)') {
                $out.Add("  Frame time per game frame in milliseconds (ordinary / replacement / ordinary retest): $($Matches[1]) / $($Matches[2]) / $($Matches[3]).")
            }
        }
    }
    if ($layerOrder) { $out.Add($layerOrder) }
    if ($recovery) {
        $names = @{
            reduce_bloom = 'reduced bloom'; current_frame_ambient_shading = 'current-frame ambient shading'
            camera_without_residual = 'camera motion without residual flow'
            directly_measured_motion = 'directly measured motion'; reduce_sharpening = 'reduced sharpening'
        }
        foreach ($line in $recovery) {
            if ($line -notmatch 'first_stage=(\w+) region=(\d+) tool=(\w+) phase=(kept|restored|no_matching_tool) ruled_out=\d+ stops=([\d./-]+)(?: reduction=([-\d.]+) total=([-\d.]+))? why=(.*)$') { continue }
            $stage, $region, $tool, $phase, $measurements, $why = $Matches[1], $Matches[2], $Matches[3], $Matches[4], $Matches[5], $Matches[8]
            if ($phase -eq 'no_matching_tool') { $out.Add("  Region $region, $stage. $why."); continue }
            $result = if ($phase -eq 'kept') { 'Kept' } else { 'Restored the previous settings after trying' }
            $out.Add("  Region $region, $stage. $result $($names[$tool]). $why. Brightness change (ordinary / trial / recheck) $measurements.")
            if ($phase -eq 'kept' -and $Matches[7] -and [double]$Matches[7] -ge 0) {
                $out.Add(('  Both corrections reduced the original flicker by {0:P1} in region {1}.' -f [double]$Matches[7], $region))
            }
        }
        if ($recovery[-1] -match 'tool=(\w+) phase=(baseline|replacement|baseline_recheck) ') {
            $out.Add("  Comparison still in progress at the last observation for '$($names[$Matches[1]])'.")
        }
    }
    if ($localRaster -match 'center=(\d+,\d+).*baseline_edges=(\d+)') {
        $out.Add("  Local jitter detected at $($Matches[2]) sampled edges. Reference point ($($Matches[1])).")
    }
    if ($jitterAction -match 'phase=(\w+) strength=(\d+).*why="([^"]+)"') {
        $out.Add("  Jitter adjustment: $($Matches[1]), strength $($Matches[2])%. $($Matches[3]).")
    }
    if ($jitterGeometry -match 'tile_xy=(\d+,\d+) samples=(\d+) phase_residual_screen_px=([-\d.]+) phase_residual_dlaa_px=([-\d.]+)(?: locked=(\d+))?') {
        $qualifier = if ($Matches[5] -eq '0') { 'No consistent jitter phase in these pairs.' } else { 'Movement follows the jitter phase.' }
        $out.Add("  Jitter-aligned edge movement measured at ($($Matches[1])) on $($Matches[2]) pairs. Screen $($Matches[3]) px, DLAA output $($Matches[4]) px. $qualifier")
    }
    $placements = @($Lines | Where-Object { $_ -match '(^|\s)native11_placement\s' })
    if ($placements) {
        $out.Add('  DX11 adaptive placement changes: ' + $placements.Count +
                 '. Last change: ' + ($placements[-1] -replace '^.*?native11_placement\s+', ''))
    }
    foreach ($s in $states) { $out.Add('  state: ' + ($s -replace '^.*?referee_state\s+', '')) }
    if (-not $windows) {
        if ($traces) { $out.Add('  A regional trace was recorded before an aggregate verdict was written.') }
        else { $out.Add('  No aggregate verdict was written. See the state lines for any recorded reason.') }
        return $out
    }
    $out.Add("  $($windows.Count) windows judged (one every few seconds).")
    $questions = [ordered]@{
        picture = 'Is the picture DLAA worked on the one that reaches the screen?'
        result  = "Did DLAA's result reach the screen?"
        motion  = 'Do the motion vectors predict the next frame?'
        jitter  = 'Did the picture shift by the jitter DLAA was told?'
    }
    foreach ($name in $questions.Keys) {
        $count = @{}; $why = @{}
        foreach ($w in $windows) {
            if ($w -match ($name + '\{verdict=(\w+)[^}]*?why="([^"]*)"\}')) {
                $v = $Matches[1]; $k = "$v`t$($Matches[2])"
                $count[$v] = 1 + [int]$count[$v]
                $why[$k] = 1 + [int]$why[$k]
            }
        }
        $judged = [int]$count['pass'] + [int]$count['partial'] + [int]$count['fail']
        $answer = if (-not $judged) { 'NOT MEASURED' }
                  elseif ([int]$count['fail'] * 2 -gt $judged) { 'NO' }
                  elseif ([int]$count['pass'] * 10 -ge $judged * 9) { 'YES' }
                  else { 'MIXED' }
        $parts = @($count.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Value)" })
        $out.Add("  $name. $($questions[$name]) $answer   ($($parts -join ', '))")
        foreach ($r in @($why.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3)) {
            $v, $reason = $r.Name -split "`t", 2
            $out.Add("      $($r.Value)x ${v}: $reason")
        }
    }
    $out.Add('  last window: ' + ($windows[-1] -replace '^.*?referee\s+(?=samples=)', ''))
    foreach ($kind in 'referee_choice', 'referee_coverage') {
        $last = $Lines | Where-Object { $_ -match "(^|\s)$kind\s" } | Select-Object -Last 1
        if ($last) { $out.Add("  ${kind}: " + ($last -replace "^.*?$kind\s+", '')) }
    }
    return $out
}
