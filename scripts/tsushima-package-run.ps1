[CmdletBinding()]
param([ValidateSet('Install','Rollback','Status','Validate')][string]$Action='Install', [string]$PackageRoot=$PSScriptRoot)
$ErrorActionPreference='Stop'
try {
    $state=Join-Path $PackageRoot 'state/deployment.json'
    if ($Action -eq 'Install' -and (Test-Path -LiteralPath $state)) {
        $receipt=Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
        if ($receipt.state -eq 'installed') {
            & (Join-Path $PackageRoot 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $PackageRoot -Action Status
            Write-Host 'This package is already installed. Use the rollback command before reinstalling.'
            exit 0
        }
    }
    & (Join-Path $PackageRoot 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $PackageRoot -Action $Action
    exit 0
} catch { Write-Error $_; exit 1 }
