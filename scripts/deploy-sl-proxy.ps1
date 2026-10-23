<#
.SYNOPSIS
  Installs, inspects or removes the NeuralAlias sl.interposer proxy in a
  Streamline game folder (C5, reconstructed 2026-10; original lost in the crash).

.DESCRIPTION
  Deploy:   game's sl.interposer.dll  -> renamed sl_interposer_real.dll
            our built sl.interposer.dll -> copied in its place
            optional NeuralAliasSL.ini  -> copied beside it (no ini = pure forwarder)
            A manifest with SHA-256 hashes of every file is written to -ManifestDir
            (repo local/, never committed) BEFORE any file is touched, then updated.
  Rollback: puts the original back ONLY if the files on disk still match the
            manifest hashes; anything unexpected is refused, never guessed.
  Status:   reports what is installed and whether it matches the manifest.

  The script refuses inconsistent states instead of "fixing" them: e.g. an
  existing sl_interposer_real.dll without our manifest (another mod?), a proxy
  already in place without a manifest, or a game file changed since deploy
  (game update). Use -WhatIf for a dry run. The game must not be running.

.EXAMPLE
  ./scripts/deploy-sl-proxy.ps1 -Action Deploy -GameDir 'D:\Steam\steamapps\common\The Last of Us Part I' `
      -ProxyDll build/Release/sl.interposer.dll -Ini deploy/NeuralAliasSL.ini
  ./scripts/deploy-sl-proxy.ps1 -Action Rollback -GameDir 'D:\...\The Last of Us Part I'
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)] [ValidateSet('Deploy', 'Rollback', 'Status')] [string] $Action,
    [Parameter(Mandatory)] [string] $GameDir,
    [string] $ProxyDll,
    [string] $Ini,
    [string] $ManifestDir = (Join-Path (Join-Path $PSScriptRoot '..') 'local')
)
$ErrorActionPreference = 'Stop'

$orig = Join-Path $GameDir 'sl.interposer.dll'
$real = Join-Path $GameDir 'sl_interposer_real.dll'
$iniDst = Join-Path $GameDir 'NeuralAliasSL.ini'
$gameKey = ((Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\') -replace '[^A-Za-z0-9]+', '_').Trim('_')
$manifestPath = Join-Path $ManifestDir "sl-proxy-deployment-$gameKey.json"

function Hash([string] $p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { $null } }
function Is-OurProxy([string] $p) {
    # Our proxy exports NeuralAliasSLProxyIdentity; check the bytes, never load the DLL.
    if (-not (Test-Path -LiteralPath $p)) { return $false }
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($p))
    return $text.Contains('NeuralAliasSLProxyIdentity')
}
function Read-Manifest { if (Test-Path -LiteralPath $manifestPath) { Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json } else { $null } }
function Write-Manifest($m) {
    New-Item -ItemType Directory -Force -Path $ManifestDir | Out-Null
    $m | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}
function Assert-GameClosed {
    $exe = Get-ChildItem -LiteralPath $GameDir -Filter *.exe -File -ErrorAction SilentlyContinue | ForEach-Object BaseName
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $exe -contains $_.ProcessName }
    if ($running) { throw "Refusing: $($running[0].ProcessName) is running. Close the game first." }
}

if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { throw "Game folder not found: $GameDir" }
$m = Read-Manifest

switch ($Action) {
    'Status' {
        [pscustomobject]@{
            GameDir = $GameDir; Manifest = $(if ($m) { $manifestPath } else { 'none' }); State = $(if ($m) { $m.state } else { 'not deployed' })
            InterposerIsOurProxy = (Is-OurProxy $orig); RealPresent = (Test-Path -LiteralPath $real); IniPresent = (Test-Path -LiteralPath $iniDst)
            InterposerMatchesManifest = $(if ($m) { (Hash $orig) -eq $m.proxy_sha256 } else { $null })
            RealMatchesManifest = $(if ($m) { (Hash $real) -eq $m.original_sha256 } else { $null })
        }
    }
    'Deploy' {
        if (-not $ProxyDll -or -not (Test-Path -LiteralPath $ProxyDll)) { throw 'Deploy needs -ProxyDll <built sl.interposer.dll>' }
        if (-not (Is-OurProxy $ProxyDll)) { throw "Refusing: $ProxyDll is not a NeuralAlias proxy build" }
        if ($Ini -and -not (Test-Path -LiteralPath $Ini)) { throw "Ini not found: $Ini" }
        Assert-GameClosed
        if ($m -and $m.state -eq 'deployed') {
            # Update in place: only allowed when everything still matches the manifest.
            if ((Hash $orig) -ne $m.proxy_sha256 -or (Hash $real) -ne $m.original_sha256) {
                throw "Refusing: files changed since the last deploy (game update or another tool). Run -Action Status."
            }
            if ($PSCmdlet.ShouldProcess($orig, 'replace proxy with new build')) {
                Copy-Item -LiteralPath $ProxyDll -Destination $orig -Force
                if ($Ini) { Copy-Item -LiteralPath $Ini -Destination $iniDst -Force }
                $m.proxy_sha256 = Hash $orig; $m.ini_sha256 = Hash $iniDst
                $m | Add-Member -NotePropertyName updated -NotePropertyValue (Get-Date).ToString('o') -Force
                Write-Manifest $m
                Write-Host "Updated proxy in $GameDir (original untouched)."
            }
            return
        }
        if (-not (Test-Path -LiteralPath $orig)) { throw "Refusing: no sl.interposer.dll in $GameDir (not a Streamline game?)" }
        if (Is-OurProxy $orig) { throw 'Refusing: a NeuralAlias proxy is already installed but no manifest exists. Restore the original manually first.' }
        if (Test-Path -LiteralPath $real) { throw 'Refusing: sl_interposer_real.dll already exists without our manifest (another mod?).' }
        if (Test-Path -LiteralPath $iniDst) { throw 'Refusing: NeuralAliasSL.ini already exists without our manifest.' }
        $new = [ordered]@{
            state = 'deploying'; game_dir = (Resolve-Path -LiteralPath $GameDir).Path; created = (Get-Date).ToString('o')
            original_sha256 = (Hash $orig); original_version = (Get-Item -LiteralPath $orig).VersionInfo.FileVersion
            proxy_sha256 = (Hash $ProxyDll); ini_sha256 = $(if ($Ini) { Hash $Ini } else { $null })
        }
        if ($PSCmdlet.ShouldProcess($GameDir, 'install NeuralAlias sl.interposer proxy')) {
            Write-Manifest ([pscustomobject]$new)           # recorded before anything moves
            Move-Item -LiteralPath $orig -Destination $real
            Copy-Item -LiteralPath $ProxyDll -Destination $orig
            if ($Ini) { Copy-Item -LiteralPath $Ini -Destination $iniDst }
            if ((Hash $real) -ne $new.original_sha256 -or (Hash $orig) -ne $new.proxy_sha256) { throw 'Post-deploy verification failed; run -Action Rollback.' }
            $new.state = 'deployed'
            Write-Manifest ([pscustomobject]$new)
            Write-Host "Deployed to $GameDir. Original preserved as sl_interposer_real.dll (SHA-256 $($new.original_sha256.Substring(0,8))...)."
        }
    }
    'Rollback' {
        if (-not $m) { throw "No manifest for this game ($manifestPath); nothing to roll back." }
        Assert-GameClosed
        $realHash = Hash $real
        if ($realHash -ne $m.original_sha256) { throw 'Refusing: sl_interposer_real.dll does not match the recorded original. Nothing changed.' }
        $curHash = Hash $orig
        if ($curHash -and $curHash -ne $m.proxy_sha256) { throw 'Refusing: sl.interposer.dll is not the proxy we installed (game update?). Nothing changed.' }
        if ($PSCmdlet.ShouldProcess($GameDir, 'restore original sl.interposer.dll')) {
            if ($curHash) { Remove-Item -LiteralPath $orig }                       # our own file
            Move-Item -LiteralPath $real -Destination $orig
            if ((Test-Path -LiteralPath $iniDst) -and (-not $m.ini_sha256 -or (Hash $iniDst) -eq $m.ini_sha256)) { Remove-Item -LiteralPath $iniDst }
            if ((Hash $orig) -ne $m.original_sha256) { throw 'Restore verification failed: original hash mismatch.' }
            $m.state = 'rolled_back'; $m | Add-Member -NotePropertyName rolled_back -NotePropertyValue (Get-Date).ToString('o') -Force
            Write-Manifest $m
            Write-Host "Restored the original sl.interposer.dll in $GameDir."
        }
    }
}
