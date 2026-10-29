$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('na-pbz-test-'+[guid]::NewGuid().ToString('N'))
$pkg=Join-Path $root 'package'; $game=Join-Path $root 'game'; $user=Join-Path $root 'user'; $state=Join-Path $root 'state'
New-Item -ItemType Directory -Force -Path $pkg,$game,$user | Out-Null
$script=Join-Path $PSScriptRoot '..\deploy-pbz.ps1'
function Check($condition,$why) { if (-not $condition) { throw $why } }
# Fixture binaries are text; no NVIDIA DLL, driver setting or game is touched.
function Get-AuthenticodeSignature { param($LiteralPath) [PSCustomObject]@{Status='Valid';SignerCertificate=[PSCustomObject]@{Subject='NVIDIA Corporation'}} }
$payload=@()
foreach($n in @('NAProfile.exe','NeuralAliasDXGI.dll','nvngx_dlss.dll','nvngx_dlssd.dll','nvngx_dlssg.dll','nvngx_dlssnr.dll')) {
    Set-Content -LiteralPath (Join-Path $pkg $n) -Value "fixture $n"
    $payload+=@{name=$n;sha256=(Get-FileHash (Join-Path $pkg $n)).Hash}
}
@{source_commit='fixture';payload=$payload} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $pkg 'package.json')
$mock=Join-Path $root 'profile.ps1'
@'
param($verb,$exe,$backup,$preset)
$global:LASTEXITCODE=0
if($verb -eq 'apply') { '[backup] complete fixture' | Set-Content -LiteralPath $backup }
'@ | Set-Content $mock
$before=@{}
foreach($n in @('platform\steam\PBZ.exe','steam\PBZ.exe','Engine\Plugins\Runtime\Nvidia\DLSS\Binaries\ThirdParty\Win64\nvngx_dlss.dll','Engine\Plugins\Runtime\Nvidia\DLSS\Binaries\ThirdParty\Win64\nvngx_dlssd.dll','Engine\Plugins\Runtime\Nvidia\StreamlineCore\Binaries\ThirdParty\Win64\nvngx_dlssg.dll')) {
    $p=Join-Path $game $n; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
    "original $n" | Set-Content -LiteralPath $p; $before[$p]=(Get-FileHash $p).Hash
}
$args=@{GameRoot=$game;PackageRoot=$pkg;StateRoot=$state;UserRoot=$user;ProfileTool=$mock}
& $script @args -Action Install
$m=Get-Content -Raw (Join-Path $state 'deployment.json') | ConvertFrom-Json
Check ($m.state -eq 'installed') 'install did not complete'
foreach($e in $m.files) { Check ((Get-FileHash $e.destination).Hash -eq $e.after_hash) 'installed hash mismatch' }
'user changed NR' | Set-Content (Join-Path $user 'NeuralAliasSL.ini')
& $script @args -Action Rollback
foreach($p in $before.Keys) { Check ((Get-FileHash $p).Hash -eq $before[$p]) 'original not restored' }
foreach($e in $m.files) { if(-not $e.before_hash) { Check (-not(Test-Path $e.destination)) 'added file remains' } }
Check (Test-Path (Join-Path $state 'last-user-settings.ini')) 'user changes not archived'
# Reinstall then simulate game update: rollback must preserve foreign bytes.
& $script @args -Action Install
$changed=Join-Path $game 'Engine\Plugins\Runtime\Nvidia\DLSS\Binaries\ThirdParty\Win64\nvngx_dlss.dll'
'game update' | Set-Content $changed
$refused=$false
try { & $script @args -Action Rollback } catch { $refused=$true }
Check $refused 'rollback overwrote game update'
Check ((Get-Content $changed) -eq 'game update') 'foreign content lost'
Copy-Item (Join-Path $pkg 'nvngx_dlss.dll') $changed -Force
& $script @args -Action Rollback
# A staging collision after profile apply must roll back all earlier writes.
$collision=Join-Path $game 'platform\steam\nvngx_dlssnr.dll.na-stage'
'unrelated stage' | Set-Content $collision
$failed=$false
try { & $script @args -Action Install } catch { $failed=$true }
Check $failed 'injected install failure did not fail'
foreach($p in $before.Keys) { Check ((Get-FileHash $p).Hash -eq $before[$p]) 'failed install did not restore original' }
Check (-not(Test-Path (Join-Path $game 'platform\steam\dxgi.dll'))) 'failed install left proxy'
Check ((Get-Content $collision) -eq 'unrelated stage') 'foreign stage overwritten'
Write-Host "PASS: install, exact rollback, mutable preferences, reinstall, changed-game refusal and partial-failure rollback. Fixture retained: $root"
