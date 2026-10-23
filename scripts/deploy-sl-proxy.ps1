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
function Fault([string] $point) {
    # Test-only fault injection (scripts/tests): simulates an interruption at a named point.
    if ($env:NA_DEPLOY_TEST_FAULT -eq $point) { throw "injected fault at $point" }
}
function Write-Manifest($m) {
    # Atomic replace (R-203): write a temp file, then rename over the manifest, so an
    # interruption leaves either the old or the new manifest, never a truncated one.
    New-Item -ItemType Directory -Force -Path $ManifestDir | Out-Null
    $tmp = "$manifestPath.tmp"
    $m | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $tmp -Encoding UTF8
    Fault 'manifest-write'
    Move-Item -LiteralPath $tmp -Destination $manifestPath -Force
}
function Set-Field($obj, [string] $name, $value) { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force }
function Field($obj, [string] $name) { if ($obj -and $obj.PSObject.Properties[$name]) { $obj.$name } else { $null } }
# Hashes of files THIS deployment wrote that may legitimately be on disk now.
function Owned-ProxyHashes($m) { @((Field $m 'proxy_sha256'), $(if ((Field $m 'state') -eq 'updating') { Field $m 'pending_proxy_sha256' })) | Where-Object { $_ } }
function Owned-IniHashes($m) {
    if (-not (Field $m 'ini_installed') -and -not ((Field $m 'state') -eq 'updating' -and (Field $m 'pending_ini_sha256'))) { return @() }
    @($(if (Field $m 'ini_installed') { Field $m 'ini_sha256' }), $(if ((Field $m 'state') -eq 'updating') { Field $m 'pending_ini_sha256' })) | Where-Object { $_ }
}
# absent | ours (we installed it and it is unchanged) | foreign (user-created or user-edited)
function Ini-State($m) {
    if (-not (Test-Path -LiteralPath $iniDst)) { return 'absent' }
    if ((Owned-IniHashes $m) -contains (Hash $iniDst)) { return 'ours' }
    return 'foreign'
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
            InterposerIsOurProxy = (Is-OurProxy $orig); RealPresent = (Test-Path -LiteralPath $real); Ini = (Ini-State $m)
            InterposerMatchesManifest = $(if ($m) { (Owned-ProxyHashes $m) -contains (Hash $orig) } else { $null })
            RealMatchesManifest = $(if ($m) { (Hash $real) -eq $m.original_sha256 } else { $null })
        }
    }
    'Deploy' {
        if (-not $ProxyDll -or -not (Test-Path -LiteralPath $ProxyDll)) { throw 'Deploy needs -ProxyDll <built sl.interposer.dll>' }
        if (-not (Is-OurProxy $ProxyDll)) { throw "Refusing: $ProxyDll is not a NeuralAlias proxy build" }
        if ($Ini -and -not (Test-Path -LiteralPath $Ini)) { throw "Ini not found: $Ini" }
        Assert-GameClosed
        if ($m -and ((Field $m 'state') -in 'deployed', 'updating')) {
            # Update in place (also completes an interrupted update). Only files this
            # deployment wrote are replaced; the original must still match.
            if ((Owned-ProxyHashes $m) -notcontains (Hash $orig) -or (Hash $real) -ne $m.original_sha256) {
                throw "Refusing: files changed since the last deploy (game update or another tool). Run -Action Status."
            }
            $iniState = Ini-State $m
            if ($Ini -and $iniState -eq 'foreign') {
                throw "Refusing: NeuralAliasSL.ini in the game folder was edited or not installed by this script; it is left untouched. Move it away to let -Ini replace it."
            }
            if ($PSCmdlet.ShouldProcess($orig, 'replace proxy with new build')) {
                # Journal first (R-203): record old and new hashes before touching any file.
                Set-Field $m 'state' 'updating'
                Set-Field $m 'pending_proxy_sha256' (Hash $ProxyDll)
                Set-Field $m 'pending_ini_sha256' $(if ($Ini) { Hash $Ini } else { $null })
                Write-Manifest $m
                Copy-Item -LiteralPath $ProxyDll -Destination $orig -Force
                Fault 'after-proxy-copy'
                if ($Ini) { Copy-Item -LiteralPath $Ini -Destination $iniDst -Force }
                $m.proxy_sha256 = $m.pending_proxy_sha256
                if ($Ini) { Set-Field $m 'ini_installed' $true; Set-Field $m 'ini_sha256' $m.pending_ini_sha256 }
                # Without -Ini the INI is untouched and its ownership record unchanged (never adopted).
                Set-Field $m 'pending_proxy_sha256' $null; Set-Field $m 'pending_ini_sha256' $null
                Set-Field $m 'state' 'deployed'; Set-Field $m 'updated' (Get-Date).ToString('o')
                Fault 'before-final-manifest'
                Write-Manifest $m
                Write-Host "Updated proxy in $GameDir (original untouched)."
            }
            return
        }
        if (-not (Test-Path -LiteralPath $orig)) { throw "Refusing: no sl.interposer.dll in $GameDir (not a Streamline game?)" }
        if (Is-OurProxy $orig) { throw 'Refusing: a NeuralAlias proxy is already installed but no manifest exists. Restore the original manually first.' }
        if (Test-Path -LiteralPath $real) { throw 'Refusing: sl_interposer_real.dll already exists without our manifest (another mod?).' }
        if ($Ini -and (Test-Path -LiteralPath $iniDst)) { throw 'Refusing: a NeuralAliasSL.ini this script did not install is already there; it is left untouched. Deploy without -Ini to keep using it, or move it away.' }
        $new = [ordered]@{
            state = 'deploying'; game_dir = (Resolve-Path -LiteralPath $GameDir).Path; created = (Get-Date).ToString('o')
            original_sha256 = (Hash $orig); original_version = (Get-Item -LiteralPath $orig).VersionInfo.FileVersion
            proxy_sha256 = (Hash $ProxyDll)
            ini_installed = [bool]$Ini; ini_sha256 = $(if ($Ini) { Hash $Ini } else { $null })
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
        if ((Field $m 'state') -eq 'deploying' -and -not $realHash -and (Hash $orig) -eq $m.original_sha256) {
            # Interrupted before the original was moved: nothing of ours is in the game folder.
            Set-Field $m 'state' 'rolled_back'; Write-Manifest $m
            Write-Host "Nothing to restore in $GameDir (the interrupted deploy never moved the original)."
            return
        }
        if ($realHash -ne $m.original_sha256) { throw 'Refusing: sl_interposer_real.dll does not match the recorded original. Nothing changed.' }
        $curHash = Hash $orig
        if ($curHash -and (Owned-ProxyHashes $m) -notcontains $curHash) { throw 'Refusing: sl.interposer.dll is not the proxy we installed (game update?). Nothing changed.' }
        $iniState = Ini-State $m
        if ($PSCmdlet.ShouldProcess($GameDir, 'restore original sl.interposer.dll')) {
            if ($curHash) { Remove-Item -LiteralPath $orig }                       # our own file (hash-verified)
            Move-Item -LiteralPath $real -Destination $orig
            # R-201: remove the INI only if this deployment installed it and it is unchanged.
            if ($iniState -eq 'ours') { Remove-Item -LiteralPath $iniDst }
            elseif ($iniState -eq 'foreign') { Write-Warning "Kept NeuralAliasSL.ini: it was edited or not installed by this script." }
            if ((Hash $orig) -ne $m.original_sha256) { throw 'Restore verification failed: original hash mismatch.' }
            $m.state = 'rolled_back'; $m | Add-Member -NotePropertyName rolled_back -NotePropertyValue (Get-Date).ToString('o') -Force
            Write-Manifest $m
            Write-Host "Restored the original sl.interposer.dll in $GameDir."
        }
    }
}
