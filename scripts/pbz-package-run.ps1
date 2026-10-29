param([ValidateSet('Install','Rollback','Status')][string]$Action='Install')
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
try { & (Join-Path $PSScriptRoot 'deploy-pbz.ps1') -Action $Action -PackageRoot $PSScriptRoot }
catch { Write-Host "FAILED: $_"; $code=1 }
finally { Stop-Transcript | Out-Null }
exit $code
