param(
    [Parameter(Mandatory=$true)][string]$LogPath,
    [Parameter(Mandatory=$true)][string]$CsvPath
)
$ErrorActionPreference = 'Stop'
# Offline only. Status samples are not atomic frame captures. Preserve menus,
# settings changes and restarts instead of labelling every gap a visual defect.
function Read-Fields([string]$Line) {
    $fields = @{}
    $Line = $Line.Split('"')[0] # prose reasons can contain key=value text
    foreach ($match in [regex]::Matches($Line, '(\w+)=(-?\d+(?:\.\d+)?)(?=\s|$)')) {
        if (!$fields.ContainsKey($match.Groups[1].Value)) {
            $fields[$match.Groups[1].Value] = [double]::Parse($match.Groups[2].Value,
                [Globalization.CultureInfo]::InvariantCulture)
        }
    }
    return $fields
}
$previous = $null
$sample = $null
$session = 1
$rows = [System.Collections.Generic.List[object]]::new()
foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $LogPath).Path)) {
    if ($line -match '^(\d\d:\d\d:\d\d) fps=') {
        $sample = @{ Time = $Matches[1]; Header = (Read-Fields $line); J2 = $null }
    } elseif ($sample -and $line.StartsWith('j2 on=')) {
        $sample.J2 = Read-Fields $line
    } elseif ($sample -and $sample.J2 -and $line.StartsWith('j2_coverage ')) {
        $coverage = Read-Fields $line
        if (!$coverage.ContainsKey('presents') -or !$coverage.ContainsKey('covered')) { continue }
        $sample.Coverage = $coverage
        if ($previous) {
            $reset = $false
            foreach ($key in @('presents','covered','later_writes','jittered')) {
                if ($coverage.ContainsKey($key) -and $previous.Coverage.ContainsKey($key) -and
                    $coverage[$key] -lt $previous.Coverage[$key]) { $reset = $true }
            }
            if ($reset) { $session++; $previous = $null }
        }
        if ($previous) {
            $presents = $coverage.presents - $previous.Coverage.presents
            $covered = $coverage.covered - $previous.Coverage.covered
            $row = [ordered]@{
                Session = $session; Start = $previous.Time; End = $sample.Time
                Presents = $presents; Covered = $covered; Gap = ($presents - $covered)
                # Endpoint samples only; neither field establishes whole-interval state.
                MenuStart = $previous.Header.menu; MenuEnd = $sample.Header.menu
                OnStart = $previous.J2.on; OnEnd = $sample.J2.on
                ColorStart = $previous.J2.color; ColorEnd = $sample.J2.color
                DepthStart = $previous.J2.depth; DepthEnd = $sample.J2.depth
                PresetStart = $previous.J2.preset; PresetEnd = $sample.J2.preset
                SharpnessStart = $previous.J2.sharpness; SharpnessEnd = $sample.J2.sharpness
            }
            foreach ($key in @('later_writes','jittered')) {
                $row[$key] = if ($coverage.ContainsKey($key) -and $previous.Coverage.ContainsKey($key)) {
                    $coverage[$key] - $previous.Coverage[$key]
                } else { $null }
            }
            foreach ($key in @('no_camera','no_state','no_selection','cuts','busy','dlss_failed','inl_wait','inl_refused')) {
                $row[$key] = if ($sample.J2.ContainsKey($key) -and $previous.J2.ContainsKey($key)) {
                    $sample.J2[$key] - $previous.J2[$key]
                } else { $null }
            }
            $row.CounterSkew = ($covered -gt $presents -or $covered -lt 0)
            $rows.Add([pscustomobject]$row)
        }
        $previous = $sample
        $sample = $null
    }
}
$rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation
Write-Output "Wrote $($rows.Count) sampled intervals across $session counter epochs to $CsvPath. Gaps are not proof of visible flicker."
