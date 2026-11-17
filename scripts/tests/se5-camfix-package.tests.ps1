# Offline transaction fixture: no installed game is read, launched or modified.
param([Parameter(Mandatory=$true)][string]$Candidate,
      [string]$Original='C:\Users\Cort\Documents\NA-SE5-camfix-original-6f6b806')
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('na-se5-combined-'+[guid]::NewGuid().ToString('N'))
$docs=Join-Path $root 'Documents';$game=Join-Path $root 'game';$evid=Join-Path $root 'evidence'
$env:LOCALAPPDATA=Join-Path $root 'LocalAppData'
New-Item -ItemType Directory -Path $docs,$game | Out-Null
$copies=@()
foreach($source in @($Original,$Candidate)) {
    $dest=Join-Path $root ([IO.Path]::GetFileName($source));New-Item -ItemType Directory -Path $dest|Out-Null
    Get-ChildItem -LiteralPath $source -File|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $dest}
    $script=Join-Path $dest 'deploy.ps1';$s=Get-Content -LiteralPath $script -Raw
    foreach($entry in @{Docs=$docs;GameDir=$game;Evid=$evid}.GetEnumerator()) {
        $m=[regex]::Match($s,('(?m)^\$'+$entry.Key+'\s*=.*$'));if(!$m.Success){throw 'Missing fixture path'}
        $s=$s.Replace($m.Value,('$'+$entry.Key+" = '"+$entry.Value+"'"))
    }
    $s=$s.Replace('try {',"function Get-Process { param(`$Name,`$ErrorAction) }`ntry {")
    [IO.File]::WriteAllText($script,$s,(New-Object Text.UTF8Encoding($false)))
    $m=Get-Content -LiteralPath (Join-Path $dest 'package.json') -Raw|ConvertFrom-Json
    foreach($f in $m.payload){$f.sha256=(Get-FileHash -LiteralPath (Join-Path $dest $f.name)).Hash}
    $m|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $dest 'package.json') -Encoding UTF8
    $copies+=,$dest
}
'fixture exe'|Set-Content -LiteralPath (Join-Path $game 'Sniper5_dx12.exe')
'native original'|Set-Content -LiteralPath (Join-Path $game 'native-original.dll')
$nativeHash=(Get-FileHash -LiteralPath (Join-Path $game 'native-original.dll')).Hash
$ps=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
function Run([int]$index,[string]$action) {
    $prefix=[IO.Path]::GetFullPath($root).TrimEnd('\')+'\'
    foreach($target in @($docs,$game,$evid)+$copies) {
        if(![IO.Path]::GetFullPath($target).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture escaped root'}
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $copies[$index] 'deploy.ps1') -Action $action *> (Join-Path $root ($index.ToString()+$action+'.log'))
    if($LASTEXITCODE -ne 0){throw "Roundtrip $index/$action failed; $root"}
}
Run 0 'Deploy'
$ini=Join-Path $game 'NeuralAliasHost.ini';$s=Get-Content -LiteralPath $ini -Raw
$s=$s.Replace('preset=L','preset=K').Replace('sharpness=100','sharpness=36').Replace('mip_bias=-1.0','mip_bias=0.0')
if([regex]::IsMatch($s,'(?m)^flow_loosen=')){$s=[regex]::Replace($s,'(?m)^flow_loosen=[^\r\n]*','flow_loosen=77')}
else{$s += "`nflow_loosen=77`n"} # j2 is the existing final section
[IO.File]::WriteAllText($ini,$s)
Run 1 'Deploy'
if((Get-FileHash -LiteralPath (Join-Path $game 'dxgi.dll')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $Candidate 'NeuralAliasDXGI.dll')).Hash){throw 'Candidate DLL mismatch'}
$s=Get-Content -LiteralPath $ini -Raw
foreach($line in @('preset=K','sharpness=36','mip_bias=0.0','flow=1','flow_loosen=77','route=auto')){if(!$s.Contains($line)){throw "Saved setting missing: $line"}}
if($s.Contains('raster_jitter=1')){throw 'Rejected later raster route carried into original baseline'}
$m=Get-Content -LiteralPath (Join-Path $evid 'manifest.json') -Raw|ConvertFrom-Json
$p=Get-Content -LiteralPath (Join-Path $Candidate 'package.json') -Raw|ConvertFrom-Json
if($m.sourceCommit -ne $p.source_commit){throw 'Wrong source receipt'}
Run 1 'Results'
Run 1 'Rollback'
if(Test-Path -LiteralPath (Join-Path $game 'dxgi.dll')){throw 'Rollback left injector'}
if((Get-FileHash -LiteralPath (Join-Path $game 'native-original.dll')).Hash -ne $nativeHash){throw 'Native file changed'}
Write-Host "PASS: $([IO.Path]::GetFileName($Original)) -> candidate -> original installation; saved choices and native file preserved. $root"
