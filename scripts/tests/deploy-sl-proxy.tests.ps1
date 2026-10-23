<#
  C5 regression for the deployment scripts, run by CI on Windows after the build.
  Uses a fake game folder: the test stub sl_interposer_real.dll plays the game's
  original sl.interposer.dll; the built proxy is installed over it and rolled back.
  Usage: deploy-sl-proxy.tests.ps1 <built sl.interposer.dll> <built sl_interposer_real.dll (stub)>
#>
param([Parameter(Mandatory)][string] $Proxy, [Parameter(Mandatory)][string] $Stub)
$ErrorActionPreference = 'Stop'
if ($PSStyle) { $PSStyle.OutputRendering = 'PlainText' }
$deploy = Join-Path (Join-Path $PSScriptRoot '..') 'deploy-sl-proxy.ps1'
$gen = Join-Path (Join-Path $PSScriptRoot '..') 'generate-sl-proxy-def.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) "na-deploy-test-$PID"
$game = Join-Path $work 'Fake Game'; $manifests = Join-Path $work 'local'
New-Item -ItemType Directory -Force $game | Out-Null
Copy-Item $Stub (Join-Path $game 'sl.interposer.dll')
Set-Content (Join-Path $work 'NeuralAliasSL.ini') "[observe]`r`nenabled=0"
$origHash = (Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash
$fail = 0
function Check($cond, $what) { if ($cond) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $script:fail++ } }
function Throws($sb, $what) { try { & $sb; Check $false "$what (expected refusal)" } catch { Check $true "$what -> refused: $($_.Exception.Message)" } }

# Dry run changes nothing.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -Ini (Join-Path $work 'NeuralAliasSL.ini') -ManifestDir $manifests -WhatIf
Check (-not (Test-Path (Join-Path $game 'sl_interposer_real.dll'))) 'WhatIf leaves the folder untouched'

# Refuses a non-proxy DLL as the "proxy".
Throws { & $deploy -Action Deploy -GameDir $game -ProxyDll $Stub -ManifestDir $manifests } 'deploy with a non-NeuralAlias DLL'

# Deploy.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -Ini (Join-Path $work 'NeuralAliasSL.ini') -ManifestDir $manifests
Check ((Get-FileHash (Join-Path $game 'sl_interposer_real.dll')).Hash -eq $origHash) 'original preserved byte-exact as sl_interposer_real.dll'
Check ((Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq (Get-FileHash $Proxy).Hash) 'proxy installed as sl.interposer.dll'
Check (Test-Path (Join-Path $game 'NeuralAliasSL.ini')) 'ini installed'
$st = & $deploy -Action Status -GameDir $game -ManifestDir $manifests
Check ($st.State -eq 'deployed' -and $st.InterposerMatchesManifest -and $st.RealMatchesManifest) 'status: deployed and consistent'

# Redeploy updates in place.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests
Check ((Get-FileHash (Join-Path $game 'sl_interposer_real.dll')).Hash -eq $origHash) 'redeploy keeps the original'

# Tampered original (simulated game update): rollback refuses and changes nothing.
$realPath = Join-Path $game 'sl_interposer_real.dll'
$bytes = [IO.File]::ReadAllBytes($realPath); $saved = $bytes.Clone(); $bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 0xFF
[IO.File]::WriteAllBytes($realPath, $bytes)
Throws { & $deploy -Action Rollback -GameDir $game -ManifestDir $manifests } 'rollback with a modified original'
Check ((Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq (Get-FileHash $Proxy).Hash) 'refused rollback left the proxy in place'
[IO.File]::WriteAllBytes($realPath, $saved)

# Rollback.
& $deploy -Action Rollback -GameDir $game -ManifestDir $manifests
Check ((Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq $origHash) 'rollback restores the original byte-exact'
Check (-not (Test-Path $realPath)) 'sl_interposer_real.dll gone after rollback'
Check (-not (Test-Path (Join-Path $game 'NeuralAliasSL.ini'))) 'our ini removed'

# Foreign sl_interposer_real.dll (another mod) blocks a fresh deploy.
Copy-Item $Stub $realPath
Throws { & $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests } 'deploy over a foreign sl_interposer_real.dll'
Remove-Item $realPath

# Generator: stub exports -> forwarders, wrapped names become comments.
$inl = Join-Path $work 'out.inl'
& $gen -Inputs "$Stub=stub" -Output $inl
$text = Get-Content -Raw $inl
Check ($text -match '/export:slIsFeatureSupported=sl_interposer_real\.slIsFeatureSupported') 'generator writes forwarders'
Check ($text -match '// slInit: typed wrapper') 'generator turns wrapped exports into comments'
Check ($text -notmatch '/export:slInit=') 'generator never forwards a wrapped export'

Remove-Item -Recurse -Force $work
if ($fail) { Write-Host "$fail check(s) failed"; exit 1 } else { Write-Host 'deploy-sl-proxy tests: ok' }
