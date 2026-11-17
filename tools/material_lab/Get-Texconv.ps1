param([string]$Destination = 'local/tools')
$ErrorActionPreference = 'Stop'
$expected = 'dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06'
$directory = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory($directory) | Out-Null
$target = Join-Path $directory 'texconv.exe'
$temporary = Join-Path $directory ('texconv-' + [guid]::NewGuid().ToString('N') + '.download')
if ((Test-Path -LiteralPath $target) -and ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected)) {
    throw 'An existing texconv has a different version. Choose a separate Destination; it has not been overwritten.'
}
if (-not (Test-Path -LiteralPath $target)) {
    try {
        Invoke-WebRequest -Uri 'https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe' -OutFile $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $expected) {
            throw 'The texconv download does not match the pinned Microsoft release.'
        }
        Move-Item -LiteralPath $temporary -Destination $target
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/microsoft/DirectXTex/may2026/LICENSE' -OutFile (Join-Path $directory 'DirectXTex-LICENSE.txt')
Write-Output "Verified texconv: $target"
