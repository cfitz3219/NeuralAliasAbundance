[CmdletBinding()]
param([ValidateSet('Install','Rollback','Status','Validate','Repair')][string]$Action='Install', [string]$PackageRoot)
$ErrorActionPreference='Stop'
if (-not $PackageRoot) { $PackageRoot = $PSScriptRoot }
try {
    $state=Join-Path $PackageRoot 'state/deployment.json'
    if ($Action -in @('Install','Repair') -and (Test-Path -LiteralPath $state)) {
        $receipt=Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
        if ($receipt.state -eq 'installed') {
            & (Join-Path $PackageRoot 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $PackageRoot -Action Status
            Write-Host 'This package is already installed. Use the rollback command before reinstalling.'
            exit 0
        }
    }
    $previousFile=Join-Path $PackageRoot 'repair-from.txt'
    if ($Action -in @('Install','Repair') -and (Test-Path -LiteralPath $previousFile)) {
        # Validate the replacement before retiring the old additive package.
        & (Join-Path $PackageRoot 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $PackageRoot -Action Validate
        $previous=(Get-Content -LiteralPath $previousFile -Raw).Trim()
        if (-not $previous -or [IO.Path]::GetFullPath($previous) -eq [IO.Path]::GetFullPath($PackageRoot)) { throw 'Invalid previous package path.' }
        $oldState=Join-Path $previous 'state/deployment.json'
        if (-not (Test-Path -LiteralPath $oldState)) { throw 'Previous package receipt is missing; installation unchanged.' }
        $old=Get-Content -LiteralPath $oldState -Raw | ConvertFrom-Json
        if ($old.state -ne 'restored') {
            & (Join-Path $previous 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $previous -Action Rollback
        }
        $Action='Install'
    } elseif ($Action -eq 'Repair') { throw 'No previous package recorded for repair.' }
    & (Join-Path $PackageRoot 'deploy-pbz.ps1') -GameProfile Tsushima -PackageRoot $PackageRoot -Action $Action
    exit 0
} catch { Write-Error $_; exit 1 }

