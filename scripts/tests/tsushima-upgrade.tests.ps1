$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('na-ghost-upgrade-'+[guid]::NewGuid().ToString('N'))
$old=Join-Path $root 'old';$new=Join-Path $root 'new'
New-Item -ItemType Directory -Path "$old/state","$new/state" -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../tsushima-package-run.ps1') -Destination $new
$old|Set-Content (Join-Path $new 'repair-from.txt')
# This fixture invokes the actual wrapper in PS5.1 against fake transactions.
# No game, NVIDIA signature/profile or installation is accessed.
$stub=@'
param($GameProfile,$PackageRoot,$Action)
$ErrorActionPreference='Stop'
if($GameProfile -ne 'Tsushima'){throw 'Wrong profile'}
Add-Content (Join-Path (Split-Path $PackageRoot) 'actions.txt') ((Split-Path $PackageRoot -Leaf)+':'+$Action)
if($Action -eq 'Validate' -and (Test-Path (Join-Path $PackageRoot 'invalid'))){throw 'fixture invalid payload'}
if($Action -eq 'Install'){@{state='installed'}|ConvertTo-Json|Set-Content (Join-Path $PackageRoot 'state/deployment.json')}
if($Action -eq 'Rollback'){@{state='restored'}|ConvertTo-Json|Set-Content (Join-Path $PackageRoot 'state/deployment.json')}
'@
foreach($dir in @($old,$new)){$stub|Set-Content (Join-Path $dir 'deploy-pbz.ps1')}
function Check($ok,$why){if(-not $ok){throw $why}}
function Reset-Fixture {
 @{state='installed'}|ConvertTo-Json|Set-Content (Join-Path $old 'state/deployment.json')
 if(Test-Path "$new/state/deployment.json"){Remove-Item -LiteralPath "$new/state/deployment.json"}
 ''|Set-Content (Join-Path $root 'actions.txt')
}
function Actions {((Get-Content (Join-Path $root 'actions.txt')|Where-Object {$_}) -join ',')}
Reset-Fixture
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$new/tsushima-package-run.ps1" -Action Install
Check ($LASTEXITCODE -eq 0) 'upgrade failed'
Check ((Actions) -eq 'new:Validate,old:Rollback,new:Install') 'upgrade order wrong'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$new/tsushima-package-run.ps1" -Action Install
Check ($LASTEXITCODE -eq 0) 'already-installed status failed'
Check ((Actions) -eq 'new:Validate,old:Rollback,new:Install,new:Status') 'repeat install mutated files'
Reset-Fixture
'bad'|Set-Content "$new/invalid"
$ErrorActionPreference='Continue'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$new/tsushima-package-run.ps1" -Action Repair 2>$null
$code=$LASTEXITCODE
$ErrorActionPreference='Stop'
Check ($code -ne 0) 'bad replacement accepted'
Check ((Actions) -eq 'new:Validate') 'old package changed before replacement validation'
Remove-Item -LiteralPath "$new/invalid"
Reset-Fixture
@{state='restored'}|ConvertTo-Json|Set-Content "$old/state/deployment.json"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$new/tsushima-package-run.ps1" -Action Repair
Check ($LASTEXITCODE -eq 0) 'repair after prior rollback failed'
Check ((Actions) -eq 'new:Validate,new:Install') 'restored old package rolled back again'
Write-Host "PASS: Ghost wrapper validation, rollback/install order, repeat-install status and restored baseline. Fixture: $root"
