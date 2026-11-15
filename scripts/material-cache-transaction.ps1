# Additive prepared-material cache transaction, called by the existing game
# installer. No game callback, network, inference, or overwrite of other entries.
function Get-MaterialEntryPath([string] $Root, [string] $Key) {
    if ($Key -notmatch '^[a-f0-9]{64}$') { throw 'Invalid prepared material identity.' }
    return Join-Path ([IO.Path]::GetFullPath($Root)) $Key
}
function Get-MaterialPackagePlan($Package, [string] $PackageRoot, [string] $CacheRoot) {
    $plan=@()
    foreach($entry in $Package.material_entries) {
        $dest=Get-MaterialEntryPath $CacheRoot $entry.key
        $source=Get-MaterialEntryPath (Join-Path $PackageRoot 'materials') $entry.key
        foreach($file in $entry.files) {
            if($file.name -notin @('manifest.json','base_color.dds','normal.dds')) { throw 'Unexpected material payload.' }
            $src=Join-Path $source $file.name
            if(!(Test-Path -LiteralPath $src) -or (Get-FileHash -LiteralPath $src).Hash -ne $file.sha256) {
                throw "Prepared material payload differs: $($entry.key)/$($file.name)"
            }
        }
        if(@($entry.files).Count -ne 3 -or @($entry.files.name | Select-Object -Unique).Count -ne 3) { throw 'Incomplete material entry.' }
        $exists=Test-Path -LiteralPath $dest
        if($exists) {
            foreach($file in $entry.files) {
                $path=Join-Path $dest $file.name
                if(!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) {
                    throw "Existing material entry differs; left untouched: $dest"
                }
            }
        }
        $plan+=@{key=$entry.key;existed=[bool]$exists;created=$false;files=@($entry.files)}
    }
    return $plan
}
function Install-MaterialEntries($Entries,[string] $PackageRoot,[string] $CacheRoot,[scriptblock] $SaveReceipt) {
    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
    foreach($entry in $Entries) {
        if($entry.existed){continue}
        $dest=Get-MaterialEntryPath $CacheRoot $entry.key
        $source=Get-MaterialEntryPath (Join-Path $PackageRoot 'materials') $entry.key
        # A concurrent publication is not ours to overwrite or claim.
        if(Test-Path -LiteralPath $dest){throw "Material appeared during install: $dest"}
        New-Item -ItemType Directory -Path $dest | Out-Null
        $entry.created=$true
        & $SaveReceipt
        foreach($file in $entry.files) {
            [IO.File]::Copy((Join-Path $source $file.name),(Join-Path $dest $file.name),$false)
        }
    }
}
function Test-MaterialRemoval($Entries,[string] $CacheRoot) {
    foreach($entry in $Entries) {
        if($entry.existed -or !$entry.created){continue}
        $dest=Get-MaterialEntryPath $CacheRoot $entry.key
        foreach($file in $entry.files) {
            if($file.name -notin @('manifest.json','base_color.dds','normal.dds')){throw 'Invalid material removal record.'}
            $path=Join-Path $dest $file.name
            if((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) {
                throw "Changed material file is retained: $path"
            }
        }
    }
}
function Remove-MaterialEntries($Entries,[string] $CacheRoot) {
    Test-MaterialRemoval $Entries $CacheRoot
    foreach($entry in $Entries) {
        if($entry.existed -or !$entry.created){continue}
        $dest=Get-MaterialEntryPath $CacheRoot $entry.key
        foreach($file in $entry.files) {
            $path=Join-Path $dest $file.name
            if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
        }
        if((Test-Path -LiteralPath $dest) -and !(Get-ChildItem -LiteralPath $dest -Force)) {
            Remove-Item -LiteralPath $dest # empty, never recursive
        }
    }
}
