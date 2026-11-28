# "Video memory: ours" for RESULTS: our own video memory item by item, next to the whole process
# figure (the game's memory and ours together) and the budget, so the game's share can be read from
# RESULTS. Reads the status log's "vram_ours" lines (host_dx12/diag/vram_ledger.hpp) and the
# "vram_item" events (one per creation or release, with the reason).
function Get-VramSummary([string[]] $lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('')
    $out.Add('Video memory: ours')
    $ours = @($lines | Where-Object { $_ -match '^vram_ours ' })
    if (-not $ours) { $out.Add('  (this build did not log it)'); return $out }
    $highest = 0.0; $highestLine = $null
    foreach ($l in $ours) { if ($l -match 'total_mb=([0-9.]+)') { $t = [double]$Matches[1]; if ($t -ge $highest) { $highest = $t; $highestLine = $l } } }
    foreach ($pair in @(@('At the end', $ours[-1]), @('At its highest', $highestLine))) {
        $l = $pair[1]
        if (-not $l) { continue }
        $total = if ($l -match 'total_mb=([0-9.]+)') { [double]$Matches[1] } else { 0 }
        $proc = if ($l -match 'process_mb=([0-9.]+)') { [double]$Matches[1] } else { $null }
        $budget = if ($l -match 'budget_mb=([0-9.]+)') { [double]$Matches[1] } else { $null }
        $out.Add(("  {0}: ours {1:N0} MB" -f $pair[0], $total) + $(if ($null -ne $proc) { ("; whole process {0:N0} MB (game and ours); the game alone about {1:N0} MB; budget {2:N0} MB" -f $proc, [math]::Max(0, $proc - $total), $budget) } else { '; whole process not sampled in this run' }))
        if ($l -match '\{([^}]*)\}') {
            foreach ($item in ($Matches[1] -split ' ' | Where-Object { $_ })) {
                $kv = $item -split '=', 2
                if ($kv.Count -eq 2) { $out.Add(("      {0,-36} {1,8:N1} MB" -f $kv[0], [double]$kv[1])) }
            }
        }
    }
    $events = @($lines | Where-Object { $_ -match ' vram_item name=' })
    if ($events) {
        $out.Add("  Made and released during the run ($($events.Count)), last 12:")
        foreach ($e in ($events | Select-Object -Last 12)) { if ($e -match 'vram_item (.*)$') { $out.Add("    $($Matches[1])") } }
    }
    return $out
}
