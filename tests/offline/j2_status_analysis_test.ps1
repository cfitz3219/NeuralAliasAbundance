$ErrorActionPreference = 'Stop'
$inputFile = [IO.Path]::GetTempFileName()
$outputFile = [IO.Path]::GetTempFileName()
try {
    @'
01:00:00 fps=60 menu=0
j2 on=1 color=42 depth=43 no_camera=2 no_state=0 cuts=1 preset=11 sharpness=0 "reason color=999"
j2_coverage presents=100 covered=90 later_writes=0 jittered=89
01:00:05 fps=60 menu=1
j2 on=1 color=42 depth=43 no_camera=3 no_state=2 cuts=2 preset=11 sharpness=50 "reason color=999"
j2_coverage presents=400 covered=389 later_writes=2 jittered=388
01:01:00 fps=60 menu=0
j2 on=1 color=42 depth=43 no_camera=0 no_state=0 cuts=0
j2_coverage presents=2 covered=1
01:01:05 fps=60 menu=0
j2 on=0 color=42 depth=43 no_camera=0 no_state=0 cuts=0
j2_coverage presents=302 covered=1
01:01:10 fps=60 menu=0
j2 on=1 color=42 depth=43 no_camera=0 no_state=0 cuts=0
j2_coverage presents=303 covered=3
01:01:15 fps=60 menu=0
j2_coverage presents=600 covered=300
'@ | Set-Content -LiteralPath $inputFile
    & "$PSScriptRoot/../../tools/analyze-j2-status.ps1" -LogPath $inputFile -CsvPath $outputFile
    $rows = @(Import-Csv -LiteralPath $outputFile)
    if ($rows.Count -ne 3) { throw 'Restart or incomplete block was counted as an interval.' }
    if ($rows[0].Gap -ne 1 -or $rows[0].no_camera -ne 1 -or $rows[0].later_writes -ne 2) {
        throw 'Incorrect counter deltas.'
    }
    if ($rows[0].ColorEnd -ne 42 -or $rows[0].MenuEnd -ne 1 -or $rows[0].SharpnessEnd -ne 50) {
        throw 'Lost context or parsed reason text as telemetry.'
    }
    if ($rows[1].Session -ne 2 -or $rows[1].OnEnd -ne 0 -or $rows[1].later_writes -ne '') {
        throw 'Restart/disabled/unknown state not preserved.'
    }
    if ($rows[2].CounterSkew -ne 'True') { throw 'Non-atomic counter skew not flagged.' }
    Write-Output 'PASS: deltas, restart, menu/settings, missing fields, prose, partial block and skew.'
} finally {
    Remove-Item -LiteralPath $inputFile, $outputFile
}
