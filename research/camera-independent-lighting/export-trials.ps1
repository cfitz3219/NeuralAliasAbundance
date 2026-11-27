function Write-LightingTrialReport {
    param([string]$Exe, [string]$Destination, [bool]$FlatMap)
    $package = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'package.json') | ConvertFrom-Json
    $python = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
        $command = Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -First 1
        if (!$command) { throw 'Python with NumPy is required for offline scoring. The original captures are saved.' }
        $python = $command.Source
    }
    $params = @((Join-Path $PSScriptRoot 'recorded_suite.py'), '--root', (Join-Path $env:LOCALAPPDATA 'NeuralAlias\captures'),
                '--exe',$Exe,'--build',$package.source_commit,'--output',(Join-Path $Destination 'lighting-tests.json'))
    if ($FlatMap) { $params += '--flat-map' }
    & $python @params | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Offline lighting report failed with exit code $LASTEXITCODE. The original captures are saved." }
}

