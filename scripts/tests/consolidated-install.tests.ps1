$ErrorActionPreference = 'Stop'
$temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $temp ('na-combined-test-' + [guid]::NewGuid().ToString('N'))
try {
    $bin = Join-Path $root 'fake-game'
    $payload = Join-Path $root 'wwii'
    New-Item -ItemType Directory -Path $bin,$payload -Force | Out-Null
    Copy-Item (Join-Path $PSScriptRoot '..\consolidated-install.ps1') $root
    $runner = Join-Path $root 'consolidated-install.ps1'
    $commit = '0000000000000000000000000000000000000001'
    $proxy = Join-Path $bin 'dxgi.dll'
    $ini = Join-Path $bin 'NeuralAliasHost.ini'
    $gameFile = Join-Path $bin 'nvngx_dlss.dll'
    $newProxy = Join-Path $payload 'NeuralAliasDXGI.dll'
    Set-Content $proxy 'prior NeuralAlias proxy'
    Set-Content $ini @('[host]','enabled=1','[observe]','producers=0','[settings]','scope=fixture')
    Set-Content $gameFile 'game-owned DLSS'
    Set-Content (Join-Path $bin 'na-test-game.exe') 'fixture executable'
    Set-Content $newProxy 'current NeuralAlias proxy'
    $old = (Get-FileHash $proxy).Hash
    $oldIni = (Get-FileHash $ini).Hash
    $gameHash = (Get-FileHash $gameFile).Hash
    $new = (Get-FileHash $newProxy).Hash
    @{source_commit=$commit; games=@(@{
        id='wwii'; label='WWII fixture'; route='B'; bin=$bin; exe='na-test-game.exe'
        processes=@('na-test-game'); known_proxies=@($old); logs=(Join-Path $root 'logs')
    })} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $root 'profiles.json')
    @{source_commit=$commit; payload=@(@{name='NeuralAliasDXGI.dll';sha256=$new})} |
        ConvertTo-Json -Depth 4 | Set-Content (Join-Path $payload 'package.json')

    & $runner -Action Validate
    & $runner -Action Rollback -Game wwii
    if ((Get-FileHash $proxy).Hash -ne $old) { throw 'Rollback touched an earlier installation.' }
    & $runner -Action Install -Game wwii
    & $runner -Action Install -Game wwii
    $installedIni = Get-Content $ini -Raw
    if ((Get-FileHash $proxy).Hash -ne $new -or $installedIni -notmatch '(?m)^graphics_api=auto\r?$' -or
        $installedIni -notmatch "(?m)^build_commit=$commit\r?$" -or
        $installedIni -notmatch '(?m)^producers=1\r?$' -or $installedIni -notmatch 'scope=fixture') {
        throw 'The combined update did not preserve and amend the existing setup.'
    }
    & $runner -Action Rollback -Game wwii
    if ((Get-FileHash $proxy).Hash -ne $old -or (Get-FileHash $ini).Hash -ne $oldIni -or
        (Get-FileHash $gameFile).Hash -ne $gameHash) { throw 'Rollback failed to restore the prior files exactly.' }

    $locked = [IO.File]::Open($ini, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $failed = $false
    try { & $runner -Action Install -Game wwii } catch { $failed = $true } finally { $locked.Dispose() }
    if (!$failed -or (Get-FileHash $proxy).Hash -ne $old -or (Get-FileHash $ini).Hash -ne $oldIni) {
        throw 'A failed second replacement left a partial installation.'
    }

    Set-Content $proxy 'unrecognized proxy'
    $unknown = (Get-FileHash $proxy).Hash
    $refused = $false
    try { & $runner -Action Install -Game wwii } catch { $refused = $_.Exception.Message -like '*unrecognized DXGI proxy*' }
    if (!$refused -or (Get-FileHash $proxy).Hash -ne $unknown) { throw 'The unknown proxy was not protected.' }
    Write-Host 'PASS: validation, install, repeat, rollback, partial replacement recovery and unknown proxy.'
} finally {
    if ((Split-Path -Parent $root) -eq $temp.TrimEnd('\') -and
        (Split-Path -Leaf $root) -match '^na-combined-test-[0-9a-f]{32}$' -and (Test-Path -LiteralPath $root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
