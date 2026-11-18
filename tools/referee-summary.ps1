# What the referee found during a run, in plain lines, read from a status log's `referee`,
# `referee_state`, `referee_choice` and `referee_coverage` event lines. Dot-source it and call
# Get-RefereeSummary with the log's lines. No game is named here; any package's results script
# can use it (deploy-native11.ps1 does, when this file sits beside it).
function Get-RefereeSummary([string[]] $Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $windows = @($Lines | Where-Object { $_ -match '(^|\s)referee\s+samples=\d+' })
    $states = @($Lines | Where-Object { $_ -match '(^|\s)referee_state\s' })
    if (-not $windows -and -not $states) {
        $out.Add('Referee: not running in this log (the Referee switch was off, or this build has no referee).')
        return $out
    }
    $out.Add('Referee (it only watches; it changes nothing shown):')
    foreach ($s in $states) { $out.Add('  state: ' + ($s -replace '^.*?referee_state\s+', '')) }
    if (-not $windows) {
        $out.Add('  No window was judged. Candidates: DLAA never ran on a frame; the run was shorter than one log interval; the referee stopped (see the state lines above).')
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
