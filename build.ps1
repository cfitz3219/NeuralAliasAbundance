$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere was not found.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio Desktop C++ tools were not found.' }
$cmakeDirectory = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$cmake = Join-Path $cmakeDirectory 'cmake.exe'
$ctest = Join-Path $cmakeDirectory 'ctest.exe'
$buildDirectory = Join-Path $PSScriptRoot 'build'
& $cmake -S $PSScriptRoot -B $buildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $buildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& $ctest --test-dir $buildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
