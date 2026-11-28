# "Jitter coverage" for RESULTS: how much of each recorded frame's scene was actually moved by the jitter DLAA
# was told about, read from the status log's `native11_jitter` event lines (one per sampled frame). A frame
# whose scene draws were told a jitter but not moved, colour draws left unmoved, or overlay meshes that stayed
# still without being tested, is what shows as shimmer on sharp detail. Dot-source it and call
# Get-JitterSummary with the log's lines. No game is named here; any package's results script can use it.
function Get-JitterSummary([string[]] $Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('')
    $out.Add('Jitter coverage')
    $rows = @($Lines | Where-Object { $_.IndexOf(' native11_jitter frame=', [StringComparison]::Ordinal) -ge 0 })
    if (-not $rows) { $out.Add('  (no native11_jitter lines in this log: this route or build did not record them)'); return $out }
    $num = { param($l, $k) if ($l -match ('(?:^|[\s{])' + [regex]::Escape($k) + '=(-?[0-9.]+)')) { [double]$Matches[1] } else { 0.0 } }
    $n = $rows.Count
    $applied = 0; $held = 0; $toldNotApplied = 0
    $shifted = 0.0; $scene = 0.0; $unshifted = 0.0; $unshiftedFrames = 0
    $lowest = 1.0; $lowestFrame = ''
    $stillDraws = 0.0; $untested = 0.0; $untestedFrames = 0; $overlays = 0.0
    $why = [ordered]@{}
    $routes = @{}; $variants = @{}
    foreach ($l in $rows) {
        $a = & $num $l 'applied'
        if ($a -ge 1) { $applied++ }
        if ((& $num $l 'held') -ge 1) { $held++ }
        if ($l -match 'told_px=(-?[0-9.]+),(-?[0-9.]+)' -and ([math]::Abs([double]$Matches[1]) + [math]::Abs([double]$Matches[2])) -gt 0 -and $a -lt 1) { $toldNotApplied++ }
        $s = & $num $l 'shifted_draws'; $d = & $num $l 'scene_draws'; $u = & $num $l 'unshifted_colour_draws'
        $shifted += $s; $scene += $d; $unshifted += $u
        if ($u -gt 0) { $unshiftedFrames++ }
        if ($a -ge 1 -and $d -gt 0 -and ($s / $d) -lt $lowest) { $lowest = $s / $d; $lowestFrame = if ($l -match 'native11_jitter frame=(\d+)') { $Matches[1] } else { '?' } }
        if ($l -match 'still_meshes\{draws=(\d+) untested=(\d+)') {
            $stillDraws += [double]$Matches[1]; $untested += [double]$Matches[2]
            if ([int]$Matches[2] -gt 0) { $untestedFrames++ }
        }
        $overlays += & $num $l 'overlays_moved'
        if ($l -match 'still_by\{([^}]*)\}') {
            foreach ($kv in ($Matches[1] -split ' ' | Where-Object { $_ -match '=' })) {
                $p = $kv -split '=', 2
                $why[$p[0]] = [double]$why[$p[0]] + [double]$p[1]
            }
        }
        if ($l -match ' route=(\S+)') { $routes[$Matches[1]] = 1 + [int]$routes[$Matches[1]] }
        if ($l -match ' variant=(\S+)') { $variants[$Matches[1]] = 1 + [int]$variants[$Matches[1]] }
    }
    $pct = { param($a, $b) if ($b -gt 0) { 100.0 * $a / $b } else { 0.0 } }
    $out.Add(("  Frames recorded: {0}; jitter applied on {1} ({2:N1} %); held (no new jitter) on {3}; told a jitter but not applied on {4}" -f $n, $applied, (& $pct $applied $n), $held, $toldNotApplied))
    $out.Add(("  Scene draws moved: {0:N0} of {1:N0} ({2:N1} %); lowest share on a jittered frame {3:N1} % (frame {4})" -f $shifted, $scene, (& $pct $shifted $scene), (100.0 * $(if ($lowestFrame) { $lowest } else { 0 })), $(if ($lowestFrame) { $lowestFrame } else { 'none' })))
    $out.Add(("  Colour draws left unmoved: {0:N0} in all, on {1} frame(s)" -f $unshifted, $unshiftedFrames))
    $out.Add(("  Overlay meshes moved with the scene: {0:N0} ({1:N1} a frame); still meshes {2:N0}, of them untested {3:N0} on {4} frame(s)" -f $overlays, ($overlays / $n), $stillDraws, $untested, $untestedFrames))
    $whyText = ($why.GetEnumerator() | Where-Object { $_.Value -gt 0 } | ForEach-Object { '{0} {1:N0}' -f $_.Key, $_.Value }) -join ', '
    $out.Add("  Why meshes stayed still: $(if ($whyText) { $whyText } else { 'none recorded' })")
    $out.Add("  Route: $(($routes.GetEnumerator() | ForEach-Object { '{0} ({1} frames)' -f $_.Key, $_.Value }) -join ', ');  variant: $(if ($variants.Count) { ($variants.GetEnumerator() | ForEach-Object { '{0} ({1} frames)' -f $_.Key, $_.Value }) -join ', ' } else { 'not logged by this build' })")
    $gaps = New-Object System.Collections.Generic.List[string]
    if ($toldNotApplied) { $gaps.Add("$toldNotApplied frame(s) told DLAA a jitter the scene never got") }
    if ($unshiftedFrames) { $gaps.Add("colour draws left unmoved on $unshiftedFrames frame(s)") }
    if ($untestedFrames) { $gaps.Add("untested still meshes on $untestedFrames frame(s)") }
    $out.Add("  Finding: $(if ($gaps.Count) { 'possible shimmer sources: ' + ($gaps -join '; ') } else { 'every recorded jittered frame moved its scene; nothing left unmoved that could shimmer' })")
    return $out
}
