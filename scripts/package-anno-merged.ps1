[CmdletBinding()]
param(
    [string]$SourcePackage = 'C:\Users\Cort\Documents\NeuralAlias-Combined-b8b6a4a',
    [string]$Destination = 'C:\Users\Cort\Documents\NeuralAlias-Anno1800-MERGED-b8b6a4a',
    [string]$RuntimeDll,
    [ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $SourcePackage 'anno'
$package = Get-Content -LiteralPath (Join-Path $source 'package.json') -Raw | ConvertFrom-Json
foreach ($file in $package.payload) {
    if ($file.name -ne [IO.Path]::GetFileName($file.name)) { throw 'Invalid payload name.' }
    if ((Get-FileHash -LiteralPath (Join-Path $source $file.name)).Hash -ne $file.sha256) { throw "Source mismatch: $($file.name)" }
}
if ([bool]$RuntimeDll -ne [bool]$SourceCommit) { throw 'Supply both RuntimeDll and SourceCommit when replacing the runtime.' }
if ($RuntimeDll -and !(Test-Path -LiteralPath $RuntimeDll -PathType Leaf)) { throw 'Replacement runtime was not found.' }
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists; preserve the existing package.' }
New-Item -ItemType Directory -Path $Destination | Out-Null
Get-ChildItem -LiteralPath $source -File | Copy-Item -Destination $Destination
$deploy = Join-Path $Destination 'deploy.ps1'
$text = [IO.File]::ReadAllText($deploy)
if ($RuntimeDll) {
    $oldBuild = [string]$package.source_commit
    if (!$oldBuild -or !$text.Contains($oldBuild)) { throw 'Template build does not match its manifest.' }
    Copy-Item -LiteralPath $RuntimeDll -Destination (Join-Path $Destination 'NeuralAliasDXGI.dll')
    $text = $text.Replace($oldBuild, $SourceCommit)
    $package.source_commit = $SourceCommit
}
$text = $text.Replace('NA-anno-layer2-log.txt', 'NeuralAlias-Anno1800-MERGED-install-log.txt')
$text = $text.Replace('NeuralAlias Anno automatic reduced-scene results', 'NeuralAlias Anno 1800 merged DLAA results')
$text = $text.Replace('NeuralAlias Anno automatic reduced-scene test', 'NeuralAlias Anno 1800 merged DLAA test')
$begin = $text.IndexOf("    Log 'READY.")
$end = $text.IndexOf('} catch {', $begin)
if ($begin -lt 0 -or $end -lt 0) { throw 'Installer guidance block not found.' }
$guidance = @'
    Log 'READY. Merged DLAA build installed. Start Anno 1800 in DX12 and load a save.'
    Write-Host 'Press Home and enable DLAA in Picture. Added DLSS upscaling is held off in this build.' -ForegroundColor Green
    Write-Host 'For the first comparison use native display resolution and game Anti-Aliasing Off.'
    Write-Host 'After closing Anno, run 03-RESULTS-Anno1800-MERGED.cmd. To remove this installation use 01-UNINSTALL-Anno1800.cmd.'
'@
$text = $text.Substring(0, $begin) + $guidance + "`r`n" + $text.Substring($end)
[IO.File]::WriteAllText($deploy, $text, [Text.UTF8Encoding]::new($false))
foreach ($file in $package.payload) { $file.sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination $file.name)).Hash }
$package | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Destination 'package.json') -Encoding UTF8
$commands = [ordered]@{
    '01-UNINSTALL-Anno1800.cmd' = 'Rollback'
    '02-INSTALL-Anno1800-MERGED.cmd' = 'Deploy'
    '03-RESULTS-Anno1800-MERGED.cmd' = 'Results'
}
foreach ($name in $commands.Keys) {
    $command = @'
@echo off
title NeuralAlias - Anno 1800 - ACTION
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" -Action ACTION
set "NA_EXIT=%ERRORLEVEL%"
if not "%NA_EXIT%"=="0" echo The operation failed. Read the message above.
echo.
pause
exit /b %NA_EXIT%
'@
    [IO.File]::WriteAllText((Join-Path $Destination $name), $command.Replace('ACTION', $commands[$name]).Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.Encoding]::ASCII)
}
@"
NeuralAlias - Anno 1800 - MERGED DLAA
Build: $($package.source_commit)

Close Anno before installing or uninstalling. Right-click the install/uninstall
CMD and choose Run as administrator if Windows needs permission.

01-UNINSTALL-Anno1800.cmd removes the CURRENT recorded NeuralAlias installation
and restores any original files in that installation's backup. It also works
after installing this package. Saved settings and logs are retained outside the game.

02-INSTALL-Anno1800-MERGED.cmd installs the merged DLAA program. It can also
replace the current recorded Anno installation directly, preserving its logs.
Start Anno normally in DX12, load a save, then press Home. Use native resolution
and Anti-Aliasing Off for the first comparison. Added upscaling stays held off.

03-RESULTS-Anno1800-MERGED.cmd collects the saved Anno logs and capture metadata
into a timestamped NA-anno-results folder in Documents after you exit the game.
Raw capture images stay at their original paths, listed in the summary.

The installer verifies all payload hashes before using them.
"@ | Set-Content -LiteralPath (Join-Path $Destination 'READ-ME-FIRST.txt') -Encoding UTF8
Write-Host "Prepared $Destination"
