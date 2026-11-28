# Read only the native DX11 draw records. Each result uses one complete game frame.
function Get-ScaleFrames([string]$StatusPath) {
    $flow = [regex]'\bnative11_flow frame=(\d+) steps=(\d+) dropped=(\d+) part=(\d+): (.*)$'
    $draw = [regex]'^\[d #\w+ fmt=\d+ (\d+)x(\d+) (.*)\]$'
    $fragments = @{}
    foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $StatusPath).Path)) {
        $m = $flow.Match($line)
        if (!$m.Success) { continue }
        $frame = [long]$m.Groups[1].Value
        if (!$fragments.ContainsKey($frame)) {
            $fragments[$frame] = @{ steps = [int]$m.Groups[2].Value; parts = @{}; invalid = $false }
        }
        $f = $fragments[$frame]
        $part = [int]$m.Groups[4].Value
        $data = $m.Groups[5].Value
        if ([int]$m.Groups[3].Value -ne 0 -or $f.steps -ne [int]$m.Groups[2].Value -or
            ($f.parts.ContainsKey($part) -and $f.parts[$part] -ne $data)) { $f.invalid = $true }
        $f.parts[$part] = $data
    }
    foreach ($frame in ($fragments.Keys | Sort-Object)) {
        $f = $fragments[$frame]
        if ($f.invalid) { continue }
        $parts = @($f.parts.Keys | Sort-Object)
        if ($parts[0] -ne 1 -or $parts[-1] -ne $parts.Count) { continue }
        $records = [regex]::Matches((($parts | ForEach-Object { $f.parts[$_] }) -join ' '), '\[[^\]]+\]')
        if ($records.Count -ne $f.steps) { continue }
        $world = @{}; $screen = @{}; $lastScene = -1; $lastScreen = -1
        for ($i = 0; $i -lt $records.Count; $i++) {
            $d = $draw.Match($records[$i].Value)
            if (!$d.Success) { continue }
            $extent = $d.Groups[1].Value + 'x' + $d.Groups[2].Value
            $rest = $d.Groups[3].Value
            if ($rest -match '\bscreen\b') { $screen[$extent] = 1; $lastScreen = $i }
            if ($rest -notmatch '\bPICTURE\b' -or $rest -notmatch '\btested=[1-9]\d*\b') { continue }
            if ($rest -match '^z#\w+=scene\((\d+x\d+),s=\d+\)' -and $Matches[1] -eq $extent) {
                $world[$extent] = 1; $lastScene = $i
            }
        }
        if ($world.Count -eq 1 -and $screen.Count -eq 1 -and $lastScene -ge 0 -and $lastScreen -gt $lastScene) {
            [pscustomobject]@{ Frame = $frame; Scene = @($world.Keys)[0]; Screen = @($screen.Keys)[0] }
        }
    }
}
