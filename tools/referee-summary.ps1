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
    $roles = @($Lines | Where-Object { $_ -match '\bnative11_lit_role frame=\d+ role_deliveries=(\d+) scene_stream=\d+' })
    $traces = @($Lines | Where-Object { $_ -match '\breferee_trace\s+frames=\d+,\d+ first_stage=(input|reconstruction|composition) ' })
    $trials = @($Lines | Where-Object { $_ -match '\bnative11_route_trial next_frame=\d+ phase=' })
    if (-not $windows -and -not $states -and -not $roles -and -not $traces -and -not $trials) {
        $out.Add('Referee: not running in this log (the Referee switch was off, or this build has no referee).')
        if ($layerOrder) { $out.Add($layerOrder) }
        return $out
    }
    $out.Add('Referee screen and delivery checks')
    if ($traces) {
        $stages = $traces | ForEach-Object { if ($_ -match '\bfirst_stage=(\w+)') { $Matches[1] } } | Group-Object
        $out.Add('  Local flicker first measured at ' + (($stages | ForEach-Object { "$($_.Name) ($($_.Count))" }) -join ', '))
        $out.Add('  Last matched pair: ' + ($traces[-1] -replace '^.*?referee_trace\s+', ''))
    }
    if ($roles) {
        $null = $roles[-1] -match 'role_deliveries=(\d+) scene_stream=(\d+)'
        $out.Add("  Rotating lighting buffers: DLAA used the screen-verified lighting route. Last checkpoint for scene $($Matches[2]): at least $($Matches[1]) submissions.")
    }
    if ($trials) {
        $null = $trials[-1] -match '\bphase=(\w+) region=(\d+) stops=([^ ]+) why=(.*)$'
        $trialState = switch ($Matches[1]) {
            'kept' { 'Replacement kept' }
            'rejected' { 'Ordinary route restored' }
            default { 'Comparison in progress at the last recorded frame' }
        }
        $out.Add("  $trialState. $($Matches[4])")
        $out.Add("  Compared brightness change in stops (ordinary / replacement / ordinary retest): $($Matches[3]).")
    }
    if ($layerOrder) { $out.Add($layerOrder) }
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
