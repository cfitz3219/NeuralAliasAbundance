param([Parameter(Mandatory=$true)][string]$PackageRoot,[ValidateSet('ser','se5')][string]$Game='ser',[ValidateSet(0,1)][int]$ExpectedFlow=0)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('na-sniper-flow-'+[guid]::NewGuid().ToString('N'))
$env:LOCALAPPDATA=Join-Path $root 'LocalAppData' # isolate optional capture/result scanning
$pkg=Join-Path $root 'package';$fakeGame=Join-Path $root 'game';$docs=Join-Path $root 'Documents';$evid=Join-Path $root 'evidence'
New-Item -ItemType Directory -Path $pkg,$fakeGame,$docs|Out-Null
Get-ChildItem -LiteralPath $PackageRoot -File|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $pkg}
$path=Join-Path $pkg 'deploy.ps1';$text=Get-Content -LiteralPath $path -Raw
foreach($entry in @{Docs=$docs;GameDir=$fakeGame;Evid=$evid}.GetEnumerator()){
    $pattern='(?m)^\$'+$entry.Key+'\s*=.*$';$match=[regex]::Match($text,$pattern)
    if(!$match.Success){throw "Missing fixture substitution $($entry.Key)"}
    $text=$text.Replace($match.Value,('$'+$entry.Key+" = '"+$entry.Value+"'"))
}
# Process discovery is fixture-local, never a real game launch/control.
$text=$text.Replace('try {',"function Get-Process { param(`$Name,`$ErrorAction) }`ntry {")
[IO.File]::WriteAllText($path,$text,(New-Object Text.UTF8Encoding($false)))
$errors=$null;$tokens=$null;[System.Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)|Out-Null
if($errors.Count){throw ($errors|Out-String)}
$manifest=Get-Content -LiteralPath (Join-Path $pkg 'package.json') -Raw|ConvertFrom-Json
foreach($f in $manifest.payload){$f.sha256=(Get-FileHash -LiteralPath (Join-Path $pkg $f.name)).Hash}
$manifest|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $pkg 'package.json') -Encoding UTF8
$exe=if($Game -eq 'ser'){'SniperResistance_dx12.exe'}else{'Sniper5_dx12.exe'}
'fixture executable'|Set-Content -LiteralPath (Join-Path $fakeGame $exe)
'original native game library'|Set-Content -LiteralPath (Join-Path $fakeGame 'original.dll')
$original=(Get-FileHash -LiteralPath (Join-Path $fakeGame 'original.dll')).Hash
$ps=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
function Run($action){
    # The transaction archives its evidence directory. Verify every substituted
    # target and the archive's parent before the child can move any directory.
    $prefix=[IO.Path]::GetFullPath($root).TrimEnd('\')+'\'
    foreach($target in @($pkg,$fakeGame,$docs,$evid,($evid+'-archive'))){
        if(![IO.Path]::GetFullPath($target).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw "Fixture target escaped root: $target"}
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File $path -Action $action *> (Join-Path $root ($action+'.log'));return $LASTEXITCODE
}
function Check($ok,$why){if(!$ok){throw $why}}
Check ((Run 'Deploy') -eq 0) 'Fixture deployment failed'
Check (Test-Path -LiteralPath (Join-Path $fakeGame 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt')) 'MIT notice not deployed'
Check ((Get-Content -LiteralPath (Join-Path $fakeGame 'NeuralAliasHost.ini') -Raw).Contains(('flow='+$ExpectedFlow))) 'Incorrect initial flow mode'
if($text.Contains("'Results'")){
    'j2compute engine=amd recorded=280 submitted=280 pairs=279 busy=0 accepted=7 total=100 ms=0.8' | Set-Content -LiteralPath (Join-Path $evid 'status-99.log')
    Check ((Run 'Results') -eq 0) 'Results export failed'
    $flowReports=@(Get-ChildItem -LiteralPath $docs -Recurse -File -Filter '*-amd-flow.txt')
    Check ($flowReports.Count -gt 0) 'AMD flow report not saved with results'
}
$installed=(Get-FileHash -LiteralPath (Join-Path $fakeGame 'dxgi.dll')).Hash
# Invalid replacement must leave the installed candidate intact.
Add-Content -LiteralPath (Join-Path $pkg 'NeuralAliasDXGI.dll') -Value 'corrupt-fixture'
Check ((Run 'Deploy') -ne 0) 'Corrupt replacement accepted'
Check ((Get-FileHash -LiteralPath (Join-Path $fakeGame 'dxgi.dll')).Hash -eq $installed) 'Invalid payload retired the previous installation'
Copy-Item -LiteralPath (Join-Path $PackageRoot 'NeuralAliasDXGI.dll') -Destination $pkg -Force
Check ((Run 'Deploy') -eq 0) 'Existing receipt upgrade failed'
Check ((Run 'Rollback') -eq 0) 'Fixture rollback failed'
Check (!(Test-Path -LiteralPath (Join-Path $fakeGame 'dxgi.dll'))) 'Our DLL left after rollback'
Check (!(Test-Path -LiteralPath (Join-Path $fakeGame 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt'))) 'Notice left after rollback'
Check ((Get-FileHash -LiteralPath (Join-Path $fakeGame 'original.dll')).Hash -eq $original) 'Native file changed'
'pre-existing native DLSS'|Set-Content -LiteralPath (Join-Path $fakeGame 'nvngx_dlss.dll')
$native=(Get-FileHash -LiteralPath (Join-Path $fakeGame 'nvngx_dlss.dll')).Hash
Check ((Run 'Deploy') -ne 0) 'Existing native DLSS was overwritten'
Check ((Get-FileHash -LiteralPath (Join-Path $fakeGame 'nvngx_dlss.dll')).Hash -eq $native) 'Native DLSS hash changed'
Write-Host "PASS: $Game prebuilt additive install, MIT notice, flow=$ExpectedFlow, corrupt replacement preservation, receipt upgrade, rollback, native-file refusal. Fixture $root"
