param([ValidateSet('Install','Rollback','Status','Repair')][string]$Action='Install')
$ErrorActionPreference='Stop'
$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($Action -ne 'Status' -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action {1}' -f $PSCommandPath,$Action
    $p=Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    $log=Join-Path $PSScriptRoot "$Action.log"
    if(Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 18 }
    exit $p.ExitCode
}
$code=0
Start-Transcript -Path (Join-Path $PSScriptRoot "$Action.log") -Append | Out-Null
try {
    $previousFile=Join-Path $PSScriptRoot 'repair-from.txt'
    $previous=if(Test-Path -LiteralPath $previousFile) { (Get-Content -Raw -LiteralPath $previousFile).Trim() } else { $null }
    if($Action -eq 'Repair') {
        if(-not $previous) { throw 'No previous package recorded for repair.' }
        # Verify replacement before removing the currently installed package.
        & (Join-Path $PSScriptRoot 'deploy-pbz.ps1') -Action Validate -PackageRoot $PSScriptRoot
        $oldManifest=Join-Path $previous 'state\deployment.json'
        if(Test-Path -LiteralPath $oldManifest) {
            $old=Get-Content -Raw -LiteralPath $oldManifest | ConvertFrom-Json
            if($old.state -ne 'restored') { & (Join-Path $previous 'deploy-pbz.ps1') -Action Rollback -PackageRoot $previous }
        }
        & (Join-Path $PSScriptRoot 'deploy-pbz.ps1') -Action Install -PackageRoot $PSScriptRoot
    } elseif($Action -eq 'Rollback' -and $previous -and -not(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'state\deployment.json'))) {
        & (Join-Path $previous 'deploy-pbz.ps1') -Action Rollback -PackageRoot $previous
    } else {
        & (Join-Path $PSScriptRoot 'deploy-pbz.ps1') -Action $Action -PackageRoot $PSScriptRoot
    }
}
catch { Write-Host "FAILED: $_"; $code=1 }
finally { Stop-Transcript | Out-Null }
exit $code
