param([Parameter(Mandatory)][string]$GameDirectory, [switch]$Update)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
$executable = Join-Path $gameRoot 'AI.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'The selected folder does not contain AI.exe.' }
if (Get-Process -Name AI -ErrorAction SilentlyContinue) { throw 'Close Alien: Isolation before installing.' }
if (!(Test-Path -LiteralPath (Join-Path $gameRoot 'dxgi.dll'))) { throw 'This diagnostic needs an existing compatible ReShade installation.' }
$stream = [IO.File]::OpenRead($executable)
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'AI.exe is not a PE executable.' }
    $stream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) {
        throw 'Expected a 32-bit x86 AI.exe.'
    }
} finally { $reader.Dispose() }
$source = Join-Path $projectRoot 'build/Win32/RelWithDebInfo/dx11_diagnostic.addon32'
if (!(Test-Path -LiteralPath $source)) { throw 'Run scripts/build.ps1 first.' }
$destination = Join-Path $gameRoot 'dx11_diagnostic.addon32'
if (Test-Path -LiteralPath $destination) {
    if (!$Update) { throw 'The diagnostic already exists. Use -Update to back up and replace a verified installation.' }
    $manifestPath = Join-Path $projectRoot 'local/deployment.json'
    if (!(Test-Path -LiteralPath $manifestPath)) { throw 'No previous deployment record. Refusing to replace an unknown file.' }
    $previous = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($previous.InstalledFile -ne $destination -or (Get-FileHash -LiteralPath $destination).Hash -ne $previous.SHA256) {
        throw 'Installed diagnostic differs from the deployment record. No changes made.'
    }
    $backupDir = Join-Path $projectRoot ('local/backup-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backupDir | Out-Null
    Copy-Item -LiteralPath $destination -Destination $backupDir
    Copy-Item -LiteralPath $manifestPath -Destination $backupDir
    Write-Output "Previous diagnostic backed up to $backupDir"
    [IO.File]::Copy($source, $destination, $true)
} else {
    [IO.File]::Copy($source, $destination, $false)
}
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $sourceHash) {
    throw 'Installed file verification failed.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'local') | Out-Null
[pscustomobject]@{
    InstalledFile = $destination
    SHA256 = $sourceHash
    GameSHA256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    InstalledAt = (Get-Date).ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'local/deployment.json')
Write-Output "Installed and verified $destination. Existing mods and settings were not changed."
