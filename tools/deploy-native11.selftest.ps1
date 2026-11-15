# Exercises deploy-native11.ps1 against a throwaway folder: install, results with no run, uninstall,
# and the refusal when the game and the program are of different kinds (32-bit vs 64-bit).
param([Parameter(Mandatory = $true)] [string] $Dll)   # a built NeuralAliasDXGI.dll, either kind
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('na-deploy-selftest-' + [Guid]::NewGuid().ToString('N'))
$pkg = Join-Path $root 'pkg'; $docs = Join-Path $root 'docs'; $runs = Join-Path $root 'runs'
New-Item -ItemType Directory -Force -Path $pkg, $docs, $runs | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'deploy-native11.ps1') -Destination $pkg
Copy-Item -LiteralPath $Dll -Destination (Join-Path $pkg 'NeuralAliasDXGI.dll')
@{ source_commit = '0123abc'; payload = @(@{ name = 'NeuralAliasDXGI.dll'; sha256 = (Get-FileHash -LiteralPath (Join-Path $pkg 'NeuralAliasDXGI.dll')).Hash }) } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $pkg 'package.json')
$fs = [IO.File]::OpenRead($Dll); $br = New-Object IO.BinaryReader($fs); $fs.Position = 0x3c; $fs.Position = $br.ReadInt32() + 4; $dllIs32 = ($br.ReadUInt16() -eq 0x014c); $fs.Dispose()
$exe32 = Join-Path $env:WINDIR 'SysWOW64\notepad.exe'; $exe64 = Join-Path $env:WINDIR 'System32\notepad.exe'
$fail = 0
function Check([bool] $ok, [string] $what) { if ($ok) { Write-Output "ok    $what" } else { Write-Output "FAIL  $what"; $script:fail++ } }
function Game([string] $name, [string] $from) { $d = Join-Path $root $name; New-Item -ItemType Directory -Force -Path $d | Out-Null; Copy-Item -LiteralPath $from -Destination (Join-Path $d 'NaSelfTestGame.exe'); $d }
function Run([string] $action, [string] $dir, [string] $label) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'deploy-native11.ps1') -Action $action -GameDir $dir -Exe 'NaSelfTestGame.exe' -Label $label -DocumentsDirectory $docs -EvidenceRoot $runs | Out-Host
    $LASTEXITCODE
}
$same = Game 'same' $(if ($dllIs32) { $exe32 } else { $exe64 })
$other = Game 'other' $(if ($dllIs32) { $exe64 } else { $exe32 })
$before = @(Get-ChildItem -LiteralPath $same -Force | ForEach-Object Name)

Check ((Run 'Deploy' $other 'other') -ne 0) 'refuses a game of the other kind'
Check (-not (Test-Path -LiteralPath (Join-Path $other 'dxgi.dll'))) 'and leaves that folder untouched'

Check ((Run 'Deploy' $same 'same') -eq 0) 'installs into a game of the same kind'
Check (Test-Path -LiteralPath (Join-Path $same 'dxgi.dll')) 'dxgi.dll is in the game folder'
$ini = Join-Path $same 'NeuralAliasHost.ini'
Check ((Test-Path -LiteralPath $ini) -and ((Get-Content -LiteralPath $ini -Raw) -match '(?m)^build_commit=0123abc\r?$')) 'settings file names the build'
Check (-not (Test-Path -LiteralPath (Join-Path $same 'nvngx_dlss.dll'))) 'no DLSS runtime is installed when the package has none'
Check ((Run 'Deploy' $same 'same') -eq 0) 'a second install replaces the first cleanly'

Check ((Run 'Results' $same 'same') -eq 0) 'results run without a play session'
$sum = Get-ChildItem -LiteralPath $docs -Recurse -Filter 'summary.txt' | Sort-Object LastWriteTime | Select-Object -Last 1
Check ($sum -and ((Get-Content -LiteralPath $sum.FullName -Raw) -match 'NO RUN LOGS')) 'and say why there are no logs'

Check ((Run 'Rollback' $same 'same') -eq 0) 'uninstalls'
$after = @(Get-ChildItem -LiteralPath $same -Force | ForEach-Object Name)
Check (-not (Compare-Object $before $after)) 'game folder is exactly as before'
Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { Write-Output "deploy-native11 selftest: $fail failed"; exit 1 }
Write-Output 'deploy-native11 selftest: all passed'
