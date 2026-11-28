# Exercises deploy-native11.ps1 against a throwaway folder: install, results with no run, uninstall,
# and the refusal when the game and the program are of different kinds (32-bit vs 64-bit).
param([Parameter(Mandatory = $true)] [string] $Dll)   # a built NeuralAliasDXGI.dll, either kind
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('na-deploy-selftest-' + [Guid]::NewGuid().ToString('N'))
$pkg = Join-Path $root 'pkg'; $docs = Join-Path $root 'docs'; $runs = Join-Path $root 'runs'
New-Item -ItemType Directory -Force -Path $pkg, $docs, $runs | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'deploy-native11.ps1') -Destination $pkg
Copy-Item -LiteralPath $Dll -Destination (Join-Path $pkg 'NeuralAliasDXGI.dll')
function Write-Package([object[]] $components) {
    @{ source_commit = '0123abc'; payload = @(@{ name = 'NeuralAliasDXGI.dll'; sha256 = (Get-FileHash -LiteralPath (Join-Path $pkg 'NeuralAliasDXGI.dll')).Hash }); components = $components } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $pkg 'package.json')
}
Write-Package @()
$fs = [IO.File]::OpenRead($Dll); $br = New-Object IO.BinaryReader($fs); $fs.Position = 0x3c; $fs.Position = $br.ReadInt32() + 4; $dllIs32 = ($br.ReadUInt16() -eq 0x014c); $fs.Dispose()
$exe32 = Join-Path $env:WINDIR 'SysWOW64\notepad.exe'; $exe64 = Join-Path $env:WINDIR 'System32\notepad.exe'
$fail = 0
function Check([bool] $ok, [string] $what) { if ($ok) { Write-Output "ok    $what" } else { Write-Output "FAIL  $what"; $script:fail++ } }
function Game([string] $name, [string] $from) { $d = Join-Path $root $name; New-Item -ItemType Directory -Force -Path $d | Out-Null; Copy-Item -LiteralPath $from -Destination (Join-Path $d 'NaSelfTestGame.exe'); $d }
function Run([string] $action, [string] $dir, [string] $label, [string] $api = 'Native11') {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'deploy-native11.ps1') -Action $action -GameDir $dir -Exe 'NaSelfTestGame.exe' -Label $label -DocumentsDirectory $docs -EvidenceRoot $runs -GraphicsApi $api | Out-Host
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
Check ((Get-Content -LiteralPath $ini -Raw) -match '(?m)^\[adapter\]\r?\nenabled=0\r?$') 'native DX11 keeps the DX12 discovery adapter off'
Check (-not (Test-Path -LiteralPath (Join-Path $same 'nvngx_dlss.dll'))) 'no DLSS runtime is installed when the package has none'
Check ((Run 'Deploy' $same 'same') -eq 0) 'a second install replaces the first cleanly'

Check ((Run 'Results' $same 'same') -eq 0) 'results run without a play session'
$sum = Get-ChildItem -LiteralPath $docs -Recurse -Filter 'summary.txt' | Sort-Object LastWriteTime | Select-Object -Last 1
Check ($sum -and ((Get-Content -LiteralPath $sum.FullName -Raw) -match 'NO RUN LOGS')) 'and say why there are no logs'

Check ((Run 'Rollback' $same 'same') -eq 0) 'uninstalls'
$after = @(Get-ChildItem -LiteralPath $same -Force | ForEach-Object Name)
Check (-not (Compare-Object $before $after)) 'game folder is exactly as before'
# The DLAA pieces and another lane's component (stand-ins: any 64-bit program and library will do).
Copy-Item -LiteralPath $exe64 -Destination (Join-Path $pkg 'NeuralAliasDlaa64.exe')
Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\version.dll') -Destination (Join-Path $pkg 'nvngx_dlss.dll')
Set-Content -LiteralPath (Join-Path $pkg 'extra.pack') -Value 'component payload'
New-Item -ItemType Directory -Force -Path (Join-Path $same 'data') | Out-Null
Write-Package @(@{ name = 'extra.pack'; sha256 = (Get-FileHash -LiteralPath (Join-Path $pkg 'extra.pack')).Hash; dest = 'data\extra.pack' })
$before = @(Get-ChildItem -LiteralPath $same -Force -Recurse | ForEach-Object FullName)
Check ((Run 'Deploy' $same 'same') -eq 0) 'installs with the DLAA pieces and a component'
Check (Test-Path -LiteralPath (Join-Path $same 'data\extra.pack')) 'the component is at its place below the game folder'
Check ((Test-Path -LiteralPath (Join-Path $same 'nvngx_dlss.dll')) -eq $true) 'the DLSS runtime is installed'
Check ((Test-Path -LiteralPath (Join-Path $same 'NeuralAliasDlaa64.exe')) -eq $dllIs32) 'the 64-bit companion is installed for a 32-bit game only'
Check ((Run 'Rollback' $same 'same') -eq 0) 'uninstalls all of it'
$after = @(Get-ChildItem -LiteralPath $same -Force -Recurse | ForEach-Object FullName)
Check (-not (Compare-Object $before $after)) 'game folder and its subfolders are exactly as before'
if (-not $dllIs32) {
    $stock = Join-Path $same 'nvngx_dlss.dll'
    Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\kernel32.dll') -Destination $stock
    $stockHash = (Get-FileHash -LiteralPath $stock).Hash
    Check ((Run 'Deploy' $same 'same') -eq 0) 'a bundled game runtime permits a private side-by-side install'
    $private = Join-Path $same 'NeuralAlias-DLSS'
    Check ((Get-FileHash -LiteralPath (Join-Path $private 'nvngx_dlss.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $pkg 'nvngx_dlss.dll')).Hash) 'the private runtime matches the package'
    Check ((Run 'Rollback' $same 'same') -eq 0) 'rolls back the private installation'
    Check (((Get-FileHash -LiteralPath $stock).Hash -eq $stockHash) -and -not (Test-Path -LiteralPath $private)) 'rollback keeps the original game DLL and removes only its own private folder'
}
Write-Package @(@{ name = 'extra.pack'; sha256 = (Get-FileHash -LiteralPath (Join-Path $pkg 'extra.pack')).Hash; dest = '..\escape.pack' })
Check ((Run 'Deploy' $same 'same') -ne 0) 'refuses a component that points outside the game folder'
Check (-not (Test-Path -LiteralPath (Join-Path $same 'dxgi.dll'))) 'and installs nothing in that case'
Write-Package @()
$auto = Game 'auto' $(if ($dllIs32) { $exe32 } else { $exe64 })
Check ((Run 'Deploy' $auto 'auto' 'Auto') -eq 0) 'installs the automatic graphics API profile'
$autoIni = Get-Content -LiteralPath (Join-Path $auto 'NeuralAliasHost.ini') -Raw
Check ($autoIni -match '(?m)^graphics_api=auto\r?$' -and
       $autoIni -match '(?m)^\[adapter\]\r?\nenabled=1\r?$' -and
       $autoIni -match '(?m)^\[discovery\]\r?\nenabled=1\r?\nread_only=1\r?$' -and
       $autoIni -match '(?m)^producers=1\r?\nearly_install=1\r?$') 'automatic DX12 can publish scene selection from early observations'
Check ((Run 'Rollback' $auto 'auto' 'Auto') -eq 0) 'uninstalls the automatic graphics API profile'
$resolved = (Resolve-Path -LiteralPath $root).ProviderPath
$temp = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).ProviderPath.TrimEnd('\')
if ((Split-Path -Parent $resolved) -eq $temp -and
    (Split-Path -Leaf $resolved) -match '^na-deploy-selftest-[a-f0-9]{32}$') {
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
}
if ($fail) { Write-Output "deploy-native11 selftest: $fail failed"; exit 1 }
Write-Output 'deploy-native11 selftest: all passed'
