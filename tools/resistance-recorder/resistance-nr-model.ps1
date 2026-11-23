[CmdletBinding()]
param(
    [ValidateSet('Install','Remove','Status')][string]$Action = 'Install',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\Sniper Elite Resistance\bin'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expected = '2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F'
$source = Join-Path $PSScriptRoot 'nvngx_dlssnr.dll'
$game = (Get-Item -LiteralPath $GameDir).FullName
if (-not [IO.File]::Exists((Join-Path $game 'SniperResistance_dx12.exe'))) {
    throw "Resistance executable missing from $game."
}
$target = Join-Path $game 'nvngx_dlssnr.dll'
$receipt = Join-Path $game 'NeuralAlias-NR-model.receipt'
$mutex = [Threading.Mutex]::new($false, 'Local\NeuralAlias-Resistance-NR-Model')
$locked = $false
function Assert-Model([string]$path) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $expected) {
        throw "Different file at $path. Its contents were preserved."
    }
}
function Assert-Closed {
    if (Get-Process -Name 'SniperResistance_dx12','SniperResistance' -ErrorAction SilentlyContinue) {
        throw 'Resistance is running. Close the game before changing its model.'
    }
}
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Another NR installation is in progress.' }
    $owned = [IO.File]::Exists($receipt)
    if ($owned) {
        $entry = [IO.File]::ReadAllText($receipt)
        if ($entry -cnotmatch ('\A' + $expected + ' ([a-f0-9]{32})\z')) { throw "Unrecognized receipt at $receipt." }
        $id = $Matches[1]
    } else { $id = [guid]::NewGuid().ToString('N') }
    $staging = Join-Path $game ("NeuralAlias-NR-" + $id + ".partial")
    $present = [IO.File]::Exists($target)
    if ($present) { Assert-Model $target }
    if ($Action -eq 'Status') {
        if ($present) { Write-Output "Verified NVIDIA NR 310.9.16.0 at $target" }
        else { Write-Output "NR model absent from $game" }
        return
    }
    Assert-Closed
    if ($Action -eq 'Remove') {
        if (-not $owned) { Write-Output 'No model owned by this installer.'; return }
        if ($present) { [IO.File]::Delete($target) }
        [IO.File]::Delete($staging)
        [IO.File]::Delete($receipt)
        Write-Output 'Removed this installation of the NR model.'
        return
    }
    if ($present) { Write-Output "NR model already verified at $target"; return }
    Assert-Model $source
    if (-not $owned) {
        # Publish ownership before copying, so a retry can finish an interrupted install.
        $scratchReceipt = Join-Path $game ("NeuralAlias-NR-" + $id + ".receipt.tmp")
        try {
            [IO.File]::WriteAllText($scratchReceipt, "$expected $id")
            [IO.File]::Move($scratchReceipt, $receipt)
        } finally { [IO.File]::Delete($scratchReceipt) }
    }
    try {
        [IO.File]::Copy($source, $staging, $true)
        Assert-Model $staging
        Assert-Closed
        [IO.File]::Move($staging, $target)
    } finally { [IO.File]::Delete($staging) }
    Write-Output "Installed and verified NVIDIA NR 310.9.16.0 at $target"
    Write-Output 'This installs the model file only. Live Resistance integration is pending.'
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
