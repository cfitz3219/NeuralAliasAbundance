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

# ---- Gate 1 R-201: user-owned INI files are never deleted or overwritten.
$ini = Join-Path $game 'NeuralAliasSL.ini'
# (a) deploy WITHOUT -Ini, user creates an INI, rollback keeps it.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests
Set-Content $ini "user settings A"
& $deploy -Action Rollback -GameDir $game -ManifestDir $manifests 3>$null
Check ((Test-Path $ini) -and ((Get-Content -Raw $ini).Trim() -eq 'user settings A')) 'R-201a: rollback keeps a user-created INI'
Check ((Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq $origHash) 'R-201a: original restored'
Remove-Item $ini
# (b) deploy WITH -Ini, user edits it, redeploy -Ini is refused, rollback keeps the edit.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -Ini (Join-Path $work 'NeuralAliasSL.ini') -ManifestDir $manifests
Set-Content $ini "user edited"
Throws { & $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -Ini (Join-Path $work 'NeuralAliasSL.ini') -ManifestDir $manifests } 'R-201b: redeploy -Ini over a user-edited INI'
Check ((Get-Content -Raw $ini).Trim() -eq 'user edited') 'R-201b: refused redeploy left the edit intact'
# (c) redeploy WITHOUT -Ini leaves the edited INI alone and does not adopt it.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests
& $deploy -Action Rollback -GameDir $game -ManifestDir $manifests 3>$null
Check ((Test-Path $ini) -and ((Get-Content -Raw $ini).Trim() -eq 'user edited')) 'R-201c: rollback keeps the user-edited INI'
Remove-Item $ini
# (d) an INI we installed and nobody touched IS removed by rollback.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -Ini (Join-Path $work 'NeuralAliasSL.ini') -ManifestDir $manifests
& $deploy -Action Rollback -GameDir $game -ManifestDir $manifests
Check (-not (Test-Path $ini)) 'R-201d: rollback removes our own unchanged INI'

# ---- Gate 1 R-203: interrupted updates stay recoverable.
$proxy2 = Join-Path $work 'proxy-v2.dll'
Copy-Item $Proxy $proxy2; Add-Content -Path $proxy2 -Value 'v2' -Encoding Ascii   # different hash, still a proxy build
foreach ($point in 'manifest-write', 'after-proxy-copy', 'before-final-manifest') {
    & $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests
    $env:NA_DEPLOY_TEST_FAULT = $point
    Throws { & $deploy -Action Deploy -GameDir $game -ProxyDll $proxy2 -ManifestDir $manifests } "R-203: update interrupted at $point"
    Remove-Item Env:NA_DEPLOY_TEST_FAULT
    $st = & $deploy -Action Status -GameDir $game -ManifestDir $manifests
    Check ($st.InterposerMatchesManifest -and $st.RealMatchesManifest) "R-203: after fault at $point the manifest still owns what is on disk"
    & $deploy -Action Rollback -GameDir $game -ManifestDir $manifests
    Check ((Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq $origHash) "R-203: rollback after fault at $point restores the original"
}
# An interrupted update can also be completed by deploying again.
& $deploy -Action Deploy -GameDir $game -ProxyDll $Proxy -ManifestDir $manifests
$env:NA_DEPLOY_TEST_FAULT = 'after-proxy-copy'
try { & $deploy -Action Deploy -GameDir $game -ProxyDll $proxy2 -ManifestDir $manifests } catch { }
Remove-Item Env:NA_DEPLOY_TEST_FAULT
& $deploy -Action Deploy -GameDir $game -ProxyDll $proxy2 -ManifestDir $manifests
$st = & $deploy -Action Status -GameDir $game -ManifestDir $manifests
Check ($st.State -eq 'deployed' -and (Get-FileHash (Join-Path $game 'sl.interposer.dll')).Hash -eq (Get-FileHash $proxy2).Hash) 'R-203: redeploy completes an interrupted update'
& $deploy -Action Rollback -GameDir $game -ManifestDir $manifests

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
