# Select capture folders by the game that produced them (Astra/Cort 2026-11-03: every game shares
# LocalAppData\NeuralAlias\captures, so results scripts picked up other games' captures).
# Since the capture_session change each folder has session.json {kind, exe, path, pid, process_started_utc}.
#
#   . .\select-captures.ps1
#   $sel = Select-NACaptures -Exe 'Anno1800' -Pids @(12600) -Since $installTimeUtc
#   $sel.Matched     # folders this game/session produced
#   $sel.Other       # tagged folders from other games or other processes (never copy them as this run's)
#   $sel.Legacy      # untagged folders (older builds): report separately as "unattributed"
function Select-NACaptures {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,   # exe name without extension, as in session.json
        [int[]]$Pids = @(),                           # this run's game pids (from status-<pid>.log names); empty = any pid
        [datetime]$Since = [datetime]::MinValue,      # folders written before this are ignored
        [string]$Root = (Join-Path $env:LOCALAPPDATA 'NeuralAlias\captures')
    )
    $matched = @(); $other = @(); $legacy = @()
    if (-not (Test-Path -LiteralPath $Root)) { return [pscustomobject]@{ Matched = $matched; Other = $other; Legacy = $legacy } }
    foreach ($d in Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue) {
        if ($d.LastWriteTimeUtc -lt $Since.ToUniversalTime()) { continue }
        $tag = Join-Path $d.FullName 'session.json'
        if (-not (Test-Path -LiteralPath $tag)) { $legacy += $d; continue }
        try { $s = Get-Content -LiteralPath $tag -Raw | ConvertFrom-Json } catch { $legacy += $d; continue }
        $sameExe = [string]::Equals([string]$s.exe, $Exe, [System.StringComparison]::OrdinalIgnoreCase)
        $samePid = ($Pids.Count -eq 0) -or ($Pids -contains [int]$s.pid)
        if ($sameExe -and $samePid) { $matched += $d } else { $other += $d }
    }
    [pscustomobject]@{ Matched = $matched; Other = $other; Legacy = $legacy }
}

# Counts, in each saved colour frame of a j2 capture (the picture before DLAA), the pixels whose
# colour is exactly zero with full alpha: what a scene surface leaves behind when it fails its own
# depth comparison (Three Kingdoms, 2026-11-21: ground patches on the samples that moved the depth
# layer furthest). The same count on every frame is black content. A count that comes and goes with
# the jitter sample is a dropout. Returns plain lines for summary.txt; reads at most -MostFrames frames.
#
#   . .\select-captures.ps1
#   Get-NABlackPixels -Capture 'C:\...\captures\j2-20261121-033409-Game-p1234'
function Get-NABlackPixels {
    param([Parameter(Mandatory = $true)][string]$Capture, [int]$MostFrames = 48)
    if (-not ('NA.BlackCount' -as [type])) {
        Add-Type -TypeDefinition @'
namespace NA {
public static class BlackCount {
    // Rows of 16-bit float RGBA. Rows are padded to a pitch except the last. -1: the file does not fit.
    public static long Half4(string path, int width, int height) {
        using (var f = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read,
                                                System.IO.FileShare.ReadWrite, 1 << 20)) {
            long row = (long)width * 8;
            if (width < 1 || height < 2 || f.Length < row * height) return -1;
            long pitch = (f.Length - row) / (height - 1);
            if (pitch < row || pitch > row + 4096 || pitch * (height - 1) + row != f.Length) return -1;
            var b = new byte[pitch];
            long n = 0;
            for (int y = 0; y < height; ++y) {
                int want = (int)(y + 1 < height ? pitch : row), got = 0;
                while (got < want) { int r = f.Read(b, got, want - got); if (r <= 0) return -1; got += r; }
                for (int i = 0; i < row; i += 8)
                    if (b[i] == 0 && (b[i + 1] & 0x7f) == 0 && b[i + 2] == 0 && (b[i + 3] & 0x7f) == 0 &&
                        b[i + 4] == 0 && (b[i + 5] & 0x7f) == 0 && b[i + 6] == 0 && b[i + 7] == 0x3c) ++n;
            }
            return n;
        }
    }
}
}
'@
    }
    $out = New-Object System.Collections.Generic.List[string]
    $meta = Get-ChildItem -LiteralPath $Capture -File -Filter '*-meta.json' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
    $frames = @(Get-ChildItem -LiteralPath $Capture -File -Filter '*-color.raw' -ErrorAction SilentlyContinue | Sort-Object Name)
    if (-not $meta -or -not $frames) { return $out }
    try { $m = Get-Content -LiteralPath $meta.FullName -Raw | ConvertFrom-Json } catch { return $out }
    $w = [int]$m.size[0]; $h = [int]$m.size[1]
    if ([int]$m.color_format -ne 10) { $out.Add("Black pixels: not counted (colour format $($m.color_format) is not 16-bit float RGBA)."); return $out }
    $jitter = @{}
    $index = Join-Path $Capture 'index.csv'
    if (Test-Path -LiteralPath $index) {
        try { foreach ($r in (Import-Csv -LiteralPath $index)) { $jitter[[int]$r.index] = ('{0:0.000},{1:0.000}' -f [double]$r.jitter_x, [double]$r.jitter_y) } } catch { }
    }
    $rows = @()
    foreach ($f in ($frames | Select-Object -First $MostFrames)) {
        if ($f.Name -notmatch '^(\d+)-color\.raw$') { continue }
        $n = [NA.BlackCount]::Half4($f.FullName, $w, $h)
        if ($n -lt 0) { continue }
        $i = [int]$Matches[1]
        $rows += [pscustomobject]@{ Frame = $i; Black = [long]$n; Jitter = $(if ($jitter.ContainsKey($i)) { $jitter[$i] } else { '?' }) }
    }
    if (-not $rows) { $out.Add('Black pixels: no colour frame could be read.'); return $out }
    $max = ($rows | Measure-Object -Property Black -Maximum).Maximum
    $min = ($rows | Measure-Object -Property Black -Minimum).Minimum
    $counted = "$($rows.Count) of $($frames.Count) colour frames counted"
    if ($max -eq 0) { $out.Add("Black pixels: none in the picture before DLAA ($counted)."); return $out }
    if ($max -lt 64) { $out.Add("Black pixels: at most $max in a frame ($counted). Too few to mean anything."); return $out }
    if ($min * 2 -ge $max) { $out.Add("Black pixels: $min to $max in every frame ($counted). Steady, so it is black content, not a dropout."); return $out }
    $bad = @($rows | Where-Object { $_.Black -ge 64 -and $_.Black * 16 -ge $max } | Sort-Object Black -Descending)
    $out.Add("BLACK DROPOUTS IN THE CAPTURE: exactly black pixels come and go between frames ($counted): $min px at the least, $max px at the most, on $($bad.Count) frames.")
    $out.Add('  Worst frames (frame: pixels, jitter x,y px): ' + (($bad | Select-Object -First 6 | ForEach-Object { "$($_.Frame): $($_.Black), $($_.Jitter)" }) -join ' | '))
    $clean = @($rows | Where-Object { $_.Black -lt 64 -or $_.Black * 16 -lt $max } | Select-Object -First 6 | ForEach-Object { "$($_.Frame): $($_.Black), $($_.Jitter)" })
    if ($clean) { $out.Add('  Clean frames for comparison: ' + ($clean -join ' | ')) }
    return $out
}
