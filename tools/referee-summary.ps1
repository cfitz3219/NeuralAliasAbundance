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
    $jitterGeometry = $Lines | Where-Object { $_ -match '\breferee_jitter_geometry frame=' } | Select-Object -Last 1
    $composite = $Lines | Where-Object { $_ -match '\breferee_composite frame=\d+ path=' } | Select-Object -Last 1
    if (-not $windows -and -not $states -and -not $roles -and -not $traces -and -not $trials -and -not $coverage -and -not $recovery -and -not $composite -and -not $jitterGeometry) {
        $out.Add('Referee: not running in this log (the Referee switch was off, or this build has no referee).')
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
    if ($roles) {
        $null = $roles[-1] -match 'role_deliveries=(\d+) scene_stream=(\d+)'
        $out.Add("  Rotating lighting buffers: DLAA used the screen-verified lighting route. Last checkpoint for scene $($Matches[2]): at least $($Matches[1]) submissions.")
    }
    if ($composite -match 'input_xy=(\d+,\d+) control_xy=(\d+,\d+) tile_size=(\d+)') {
        $out.Add("  Repeated regional difference. A $($Matches[3])-pixel tile at ($($Matches[1])) matched the input. A control at ($($Matches[2])) matched the reconstructed picture.")
    }
    if ($trials -and $trials[-1] -match '\bphase=(\w+) region=(\d+) stops=([^ ]+)(?: regional=(\d+))?(?: verified_reduction=([-\d.]+))? why=(.*)$') {
        $regional = $Matches[4] -eq '1'
        $why = $Matches[6]
        $stops = $Matches[3]
        $trialState = switch ($Matches[1]) {
            'kept' { 'Replacement kept' }
            'rejected' { 'Ordinary route restored' }
            default { 'Comparison in progress at the last recorded frame' }
        }
        $out.Add("  $trialState. $why")
        if ($regional) { $out.Add('  The comparison follows the same disputed tiles and clear controls through the ordinary route, replacement, and ordinary retest.') }
        else { $out.Add("  Compared brightness change in stops (ordinary / replacement / ordinary retest): $stops.") }
        if ($Matches[1] -eq 'kept' -and $Matches[5] -and [double]$Matches[5] -ge 0) {
            $reduction = [double]$Matches[5]
            $goal = if ($reduction -ge .90) { 'met the 90% target' } else { 'below the 90% target' }
            $out.Add(('  Measured {0:P1} less brightness variation in the affected region, {1}.' -f $reduction, $goal))
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
                $out.Add(('  Both corrections reduced the original flicker by {0:P1} in the same region.' -f [double]$Matches[7]))
            }
        }
        if ($recovery[-1] -match 'tool=(\w+) phase=(baseline|replacement|baseline_recheck) ') {
            $out.Add("  Comparison still in progress at the last observation for '$($names[$Matches[1]])'.")
        }
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
