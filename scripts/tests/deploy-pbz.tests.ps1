$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('na-pbz-test-'+[guid]::NewGuid().ToString('N'))
$pkg=Join-Path $root 'package'; $game=Join-Path $root 'game'; $user=Join-Path $root 'user'; $state=Join-Path $root 'state'
New-Item -ItemType Directory -Force -Path $pkg,$game,$user | Out-Null
$script=Join-Path $PSScriptRoot '..\deploy-pbz.ps1'
function Check($condition,$why) { if (-not $condition) { throw $why } }
# Fixture binaries are text; no NVIDIA DLL, driver setting or game is touched.
# Process discovery is also fixture-local; a real game running elsewhere must
# not prevent install/rollback tests against these temporary text files.
$global:naFixtureRunning=$false
function Get-Process { [CmdletBinding()] param([string]$Name) if($global:naFixtureRunning){[PSCustomObject]@{Name=$Name}} }
function Get-AuthenticodeSignature { param($LiteralPath) [PSCustomObject]@{Status='Valid';SignerCertificate=[PSCustomObject]@{Subject='NVIDIA Corporation'}} }
$payload=@()
foreach($n in @('NAProfile.exe','NeuralAliasDXGI.dll','nvngx_dlssnr.dll','Poise-Regular.ttf','Poise-Medium.ttf','Poise-OFL.txt')) {
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
# Reproduce fc04ba5 packaging failure: a transcript entered the payload manifest.
# Preflight must reject it without touching the current installation/receipt.
$packagePath=Join-Path $pkg 'package.json'
$validManifest=Get-Content $packagePath -Raw
$badManifest=$validManifest | ConvertFrom-Json
$badManifest.payload += [PSCustomObject]@{name='Repair.log';sha256='not-a-payload'}
$badManifest | ConvertTo-Json -Depth 4 | Set-Content $packagePath
$logStream=[IO.File]::Open((Join-Path $pkg 'Repair.log'),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
$refusedLog=$false
try { & $script @args -Action Validate } catch { $refusedLog=$true; Check ($_.Exception.Message -like '*Not a package payload*') 'wrong transcript rejection' } finally { $logStream.Dispose() }
Check $refusedLog 'transcript accepted as payload'
Check ((Get-Content (Join-Path $state 'deployment.json') -Raw | ConvertFrom-Json).state -eq 'installed') 'preflight changed installation state'
$validManifest | Set-Content $packagePath
$global:naFixtureRunning=$true
& $script @args -Action Validate
foreach($action in @('Install','Rollback')){
 $blocked=$false
 try { & $script @args -Action $action } catch { $blocked=$_.Exception.Message -like 'Close *' }
 Check $blocked 'mutating action accepted while fixture process running'
}
$global:naFixtureRunning=$false
foreach($e in $m.files) { Check ((Get-FileHash $e.destination).Hash -eq $e.after_hash) 'installed hash mismatch' }
# Additive only: every file the game shipped is byte-identical while installed.
foreach($p in $before.Keys) { Check ((Get-FileHash $p).Hash -eq $before[$p]) 'install changed a game file' }
foreach($e in $m.files) { if($e.destination.StartsWith($game)) { Check (-not $e.before_hash) 'install replaced an existing game file' } }
'user changed NR' | Set-Content (Join-Path $user 'NeuralAliasSL.ini')
& $script @args -Action Rollback
foreach($p in $before.Keys) { Check ((Get-FileHash $p).Hash -eq $before[$p]) 'original not restored' }
foreach($e in $m.files) { if(-not $e.before_hash) { Check (-not(Test-Path $e.destination)) 'added file remains' } }
Check (Test-Path (Join-Path $state 'last-user-settings.ini')) 'user changes not archived'
# Reinstall, then another tool overwrites our dxgi.dll: rollback must preserve foreign bytes.
& $script @args -Action Install
$changed=Join-Path $game 'platform\steam\dxgi.dll'
'another mod' | Set-Content $changed
$refused=$false
try { & $script @args -Action Rollback } catch { $refused=$true }
Check $refused 'rollback deleted a file another tool wrote'
Check ((Get-Content $changed) -eq 'another mod') 'foreign content lost'
Copy-Item (Join-Path $pkg 'NeuralAliasDXGI.dll') $changed -Force
& $script @args -Action Rollback
# A file the installer would add already exists in the game folder: refuse, change nothing.
$existing=Join-Path $game 'platform\steam\dxgi.dll'
'game or other mod dxgi' | Set-Content $existing
$snapshot=Get-ChildItem -Recurse -File $game | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName).Hash)" }
$refusedExisting=$false
try { & $script @args -Action Install } catch { $refusedExisting=$true; Check ($_.Exception.Message -like '*never replaces*') 'wrong refusal' }
Check $refusedExisting 'install replaced an existing dxgi.dll'
$after=Get-ChildItem -Recurse -File $game | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName).Hash)" }
Check ((Compare-Object $snapshot $after) -eq $null) 'refused install changed the game folder'
Remove-Item -LiteralPath $existing -Force
# A staging collision after profile apply must roll back all earlier writes.
$collision=Join-Path $game 'platform\steam\nvngx_dlssnr.dll.na-stage'
'unrelated stage' | Set-Content $collision
$failed=$false
try { & $script @args -Action Install } catch { $failed=$true; Check ($_.Exception.Message -like '*Unfinished staging file*') 'failure occurred before the intended staging collision' }
Check $failed 'injected install failure did not fail'
foreach($p in $before.Keys) { Check ((Get-FileHash $p).Hash -eq $before[$p]) 'failed install did not restore original' }
Check (-not(Test-Path (Join-Path $game 'platform\steam\dxgi.dll'))) 'failed install left proxy'
Check ((Get-Content $collision) -eq 'unrelated stage') 'foreign stage overwritten'
Write-Host "PASS: additive-only install, exact rollback, mutable preferences, reinstall, foreign-file refusal, existing-file refusal and partial-failure rollback. Fixture retained: $root"

# Ghost shares the tested transaction but has root-level binaries, separate
# preferences and no Unreal plugin directory or driver-profile modification.
$ghost=Join-Path $root 'ghost'; $ghostState=Join-Path $root 'ghost-state'; $ghostUser=Join-Path $root 'ghost-user'
New-Item -ItemType Directory -Force -Path $ghost,$ghostUser | Out-Null
$native=@{}
foreach($name in @('GhostOfTsushima.exe','sl.interposer.dll','nvngx_dlss.dll','nvngx_dlssg.dll')) {
    $path=Join-Path $ghost $name; "native $name" | Set-Content $path; $native[$path]=(Get-FileHash $path).Hash
}
$priorPrefs=Join-Path $ghostUser 'NeuralAliasSL.ini'
'old scoped choices' | Set-Content $priorPrefs
$priorHash=(Get-FileHash $priorPrefs).Hash
$ghostArgs=@{GameProfile='Tsushima';GameRoot=$ghost;PackageRoot=$pkg;StateRoot=$ghostState;UserRoot=$ghostUser;ProfileTool=(Join-Path $root 'must-not-run.exe')}
& $script @ghostArgs -Action Validate
& $script @ghostArgs -Action Install
$gm=Get-Content (Join-Path $ghostState 'deployment.json') -Raw | ConvertFrom-Json
Check ($gm.files.Count -eq 7) 'Ghost installed an unexpected file set'
Check (-not $gm.profile_attempted) 'Ghost altered a driver profile'
Check (Test-Path (Join-Path $ghost 'dxgi.dll')) 'Ghost proxy absent'
Check (-not (Test-Path (Join-Path $ghost 'Engine'))) 'Ghost got an Unreal directory'
Check ((Get-Content $priorPrefs -Raw) -match 'scope=Tsushima') 'Ghost scope incorrect'
Check ((Get-Content $priorPrefs -Raw) -match 'profile_override=0') 'Ghost profile override enabled'
foreach($path in $native.Keys) { Check ((Get-FileHash $path).Hash -eq $native[$path]) 'Ghost native file changed' }
& $script @ghostArgs -Action Rollback
Check ((Get-FileHash $priorPrefs).Hash -eq $priorHash) 'Ghost prior preferences not restored'
Check (-not (Test-Path (Join-Path $ghost 'dxgi.dll'))) 'Ghost rollback left proxy'
foreach($path in $native.Keys) { Check ((Get-FileHash $path).Hash -eq $native[$path]) 'Ghost rollback changed native file' }
'legacy proxy original' | Set-Content (Join-Path $ghost 'sl_interposer_real.dll')
$legacyRefused=$false
try { & $script @ghostArgs -Action Install } catch { $legacyRefused=$_.Exception.Message -like '*previous Streamline*' }
Check $legacyRefused 'Ghost accepted a second injector over the legacy proxy'
Write-Host 'PASS: Ghost additive install/rollback, original preferences restored, no native DLL or driver-profile changes, legacy proxy refusal.'


