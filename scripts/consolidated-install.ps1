[CmdletBinding()]
param(
    [ValidateSet('Install','Rollback','Status','Results','Validate')][string]$Action,
    [string]$Game
)

$ErrorActionPreference = 'Stop'
$index = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'profiles.json') -Raw | ConvertFrom-Json

function Hash([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Set-IniKeys([string]$Text, [string]$Section, [System.Collections.IDictionary]$Values) {
    $sectionPattern = '(?im)^\[' + [regex]::Escape($Section) + '\][ \t]*\r?$'
    $header = [regex]::Match($Text, $sectionPattern)
    if (!$header.Success) { throw "The existing NeuralAlias configuration is missing [$Section]." }
    $start = $header.Index + $header.Length
    $tail = $Text.Substring($start)
    $next = [regex]::Match($tail, '(?m)^\[')
    $length = if ($next.Success) { $next.Index } else { $tail.Length }
    $body = $tail.Substring(0, $length).TrimStart("`n", "`r")
    $lines = foreach ($key in $Values.Keys) {
        $body = [regex]::Replace($body, '(?im)^[ \t]*' + [regex]::Escape($key) + '[ \t]*=.*(?:\r?\n|$)', '')
        "$key=$($Values[$key])"
    }
    $Text.Substring(0, $header.Index) + "[$Section]`r`n" + ($lines -join "`r`n") + "`r`n" + $body + $tail.Substring($length)
}

function Replace-OwnedFile([string]$Source, [string]$Destination) {
    # Stage alongside the destination so replacement is atomic on its volume.
    $temporary = $Destination + '.na-' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary
        [IO.File]::Replace($temporary, $Destination, [NullString]::Value)
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary }
    }
}

function Read-Receipt([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
}

function Write-Receipt($Receipt, [string]$Path) {
    $temp = "$Path.tmp"
    $Receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temp -Encoding UTF8
    Move-Item -LiteralPath $temp -Destination $Path -Force
}

function Assert-Package($Profile, [string]$Directory) {
    $package = Get-Content -LiteralPath (Join-Path $Directory 'package.json') -Raw | ConvertFrom-Json
    if ($package.source_commit -ne $index.source_commit) { throw 'Package build identifiers differ.' }
    foreach ($item in $package.payload) {
        if ($item.name -ne [IO.Path]::GetFileName($item.name) -or (Hash (Join-Path $Directory $item.name)) -ne $item.sha256) {
            throw "Package file failed verification: $($item.name)"
        }
    }
    $proxy = Join-Path $Profile.bin 'dxgi.dll'
    $prior = Hash $proxy
    $new = Hash (Join-Path $Directory 'NeuralAliasDXGI.dll')
    if (!$new) { throw 'The shared NeuralAlias runtime is missing.' }
    if ($prior -and $prior -ne $new -and $prior -notin $Profile.known_proxies) {
        throw "An unrecognized DXGI proxy exists at $proxy. Existing files were left in place."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Profile.bin $Profile.exe) -PathType Leaf)) {
        throw "$($Profile.label) executable was not found."
    }
}

function Restore-Upgrade($Receipt, [string]$Path, $Profile) {
    foreach ($file in $Receipt.files) {
        $current = Hash (Join-Path $Profile.bin $file.name)
        if ($current -notin @($file.before, $file.after) -or (Hash $file.backup) -ne $file.before) {
            throw "Rollback stopped because $($file.name) or its backup changed after installation."
        }
    }
    foreach ($file in $Receipt.files) {
        $target = Join-Path $Profile.bin $file.name
        if ((Hash $target) -ne $file.before) { Replace-OwnedFile $file.backup $target }
    }
    $Receipt.state = 'restored'
    Write-Receipt $Receipt $Path
}

function Invoke-Game($Profile) {
    $directory = Join-Path $PSScriptRoot $Profile.id
    $proxy = Join-Path $Profile.bin 'dxgi.dll'
    $ini = Join-Path $Profile.bin 'NeuralAliasHost.ini'
    $state = Join-Path $directory 'consolidated-state'
    $receiptPath = Join-Path $state 'upgrade.json'
    $freshPath = Join-Path $state 'fresh-install.txt'
    $receipt = Read-Receipt $receiptPath
    if ($Action -eq 'Validate') {
        Assert-Package $Profile $directory
        Write-Host "$($Profile.label) verified"
        return
    }
    if ($Action -eq 'Status') {
        $current = Hash $proxy
        $ours = Hash (Join-Path $directory 'NeuralAliasDXGI.dll')
        $message = if ($current -eq $ours) { "build $($index.source_commit) installed" }
                   elseif ($current -in $Profile.known_proxies) { 'previous NeuralAlias build installed' }
                   elseif (!$current) { 'no DXGI proxy installed' } else { 'other DXGI proxy installed' }
        Write-Host "$($Profile.label) - $message"
        return
    }
    if ($Action -eq 'Results') {
        $buildLine = "NeuralAlias build $($index.source_commit)"
        $run = Get-ChildItem -LiteralPath $Profile.logs -File -Filter 'startup-*.log' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending |
            Where-Object { (Get-Content -LiteralPath $_.FullName -TotalCount 1) -like "*$buildLine" } |
            Select-Object -First 1
        if (!$run) { Write-Host 'No run logs for this build yet.'; return }
        $suffix = $run.Name.Substring('startup'.Length)
        $dest = Join-Path ([Environment]::GetFolderPath('MyDocuments')) ("NA-" + $Profile.id + "-results-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
        New-Item -ItemType Directory -Path $dest | Out-Null
        foreach ($name in 'startup','status','observe') {
            $log = Join-Path $Profile.logs ($name + $suffix)
            if (Test-Path -LiteralPath $log -PathType Leaf) { Copy-Item -LiteralPath $log -Destination $dest }
        }
        Write-Host "Run logs saved to $dest"
        return
    }
    if (Get-Process -Name $Profile.processes -ErrorAction SilentlyContinue) {
        throw "Close $($Profile.label) before $($Action.ToLowerInvariant())."
    }
    if ($Action -eq 'Rollback' -and $receipt -and $receipt.state -ne 'restored') {
        Restore-Upgrade $receipt $receiptPath $Profile
        Write-Host "$($Profile.label) returned to its previous NeuralAlias installation."
        return
    }
    if ($Action -eq 'Rollback' -and !(Test-Path -LiteralPath $freshPath -PathType Leaf)) {
        Write-Host 'This package has no installation to roll back.'
        return
    }
    if ($Action -eq 'Install') {
        Assert-Package $Profile $directory
        $prior = Hash $proxy
        $new = Hash (Join-Path $directory 'NeuralAliasDXGI.dll')
        if ($receipt -and $receipt.state -eq 'preparing') {
            Restore-Upgrade $receipt $receiptPath $Profile
            $prior = Hash $proxy
        }
        if ($prior -eq $new) { Write-Host "$($Profile.label) already has this build."; return }
        if ($prior) {
            if (!(Test-Path -LiteralPath $ini -PathType Leaf)) { throw 'The existing proxy has no NeuralAlias configuration.' }
            $text = Set-IniKeys ([IO.File]::ReadAllText($ini)) 'host' ([ordered]@{graphics_api='auto';build_commit=$index.source_commit})
            if ($Profile.route -eq 'B') { $text = Set-IniKeys $text 'observe' ([ordered]@{producers=1}) }
            $backup = Join-Path $state ([guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
            $newIni = Join-Path $backup 'next.ini'
            [IO.File]::WriteAllText($newIni, $text, [Text.Encoding]::Unicode)
            Copy-Item -LiteralPath $proxy,$ini -Destination $backup
            $receipt = [PSCustomObject]@{state='preparing'; files=@(
                @{name='dxgi.dll'; backup=(Join-Path $backup 'dxgi.dll'); before=$prior; after=$new},
                @{name='NeuralAliasHost.ini'; backup=(Join-Path $backup 'NeuralAliasHost.ini'); before=(Hash $ini); after=(Hash $newIni)}
            )}
            Write-Receipt $receipt $receiptPath
            try {
                Replace-OwnedFile (Join-Path $directory 'NeuralAliasDXGI.dll') $proxy
                Replace-OwnedFile $newIni $ini
                $receipt.state = 'installed'
                Write-Receipt $receipt $receiptPath
            } catch {
                Restore-Upgrade $receipt $receiptPath $Profile
                throw
            }
            Write-Host "$($Profile.label) updated. Start it normally, then press Home in gameplay."
            return
        }
    }
    # A fresh install uses the existing per-game additive transaction and its rollback.
    if ($Profile.route -eq 'A') {
        & (Join-Path $directory 'deploy-pbz.ps1') -Action $Action -GameProfile $Profile.game_profile -PackageRoot $directory
    } else {
        $verb = if ($Action -eq 'Install') { 'Deploy' } else { 'Rollback' }
        & (Join-Path $directory 'deploy.ps1') -Action $verb
    }
    if ($LASTEXITCODE) { throw "The per-game transaction returned exit code $LASTEXITCODE." }
    if ($Action -eq 'Install') {
        if ((Hash $proxy) -ne (Hash (Join-Path $directory 'NeuralAliasDXGI.dll'))) {
            throw 'The per-game installer did not install this build.'
        }
        New-Item -ItemType Directory -Path $state -Force | Out-Null
        Set-Content -LiteralPath $freshPath -Value $index.source_commit -Encoding ASCII
    } else {
        Remove-Item -LiteralPath $freshPath
    }
}

Write-Host "NeuralAlias combined build $($index.source_commit)"
if ($Action -eq 'Validate' -and !$Game) {
    foreach ($profile in $index.games) { Invoke-Game $profile }
    return
}
if (!$Game) {
    for ($i=0; $i -lt $index.games.Count; $i++) { Write-Host "$($i+1). $($index.games[$i].label)" }
    $choice = Read-Host 'Choose a game number, or press Enter to quit'
    if (!$choice) { return }
    if ($choice -notmatch '^[1-6]$' -or [int]$choice -gt $index.games.Count) { throw 'Invalid game selection.' }
    $Game = $index.games[[int]$choice-1].id
}
if (!$Action) {
    switch (Read-Host 'Enter I to install, R to roll back, S for status or L to save run logs') {
        'i' { $Action='Install' }
        'r' { $Action='Rollback' }
        's' { $Action='Status' }
        'l' { $Action='Results' }
        default { return }
    }
}
$selected = @($index.games | Where-Object id -eq $Game)
if ($selected.Count -ne 1) { throw "Unknown game profile: $Game" }
Invoke-Game $selected[0]
