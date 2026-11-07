param([Parameter(Mandatory)][string]$GameDirectory, [switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$gameRoot=(Resolve-Path -LiteralPath $GameDirectory).Path
if(Get-Process -Name AI -ErrorAction SilentlyContinue){throw 'Close Alien: Isolation before installing.'}
$helper=Join-Path $projectRoot 'build/x64/RelWithDebInfo/neural_helper.exe'
$runtime=Join-Path $projectRoot 'local/neural-runtime-310.8.0'
$dll=Join-Path $runtime 'nvngx_dlssnr.dll'
$shader=Join-Path $projectRoot 'shaders/DX11_Neural.fx'
$addon=Join-Path $projectRoot 'build/Win32/RelWithDebInfo/dx11_diagnostic.addon32'
$destination=Join-Path $gameRoot 'reshade-shaders/Shaders/DX11_Neural.fx'
$config=Join-Path $gameRoot 'dx11-neural-paths.txt'
$manifest=Join-Path $projectRoot 'local/neural-trial-deployment.json'
foreach($file in @($helper,$dll,$shader,$addon,(Join-Path $gameRoot 'dx11-dlaa-paths.txt'))){
    if(!(Test-Path -LiteralPath $file)){throw "Required file missing: $file"}
}
$signature=Get-AuthenticodeSignature -LiteralPath $dll
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA'){
    throw 'NVIDIA runtime signature verification failed.'
}
if((Get-FileHash -LiteralPath $dll).Hash -ne 'E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E'){
    throw 'Neural runtime differs from the tested version.'
}
$previous=$null
if(Test-Path -LiteralPath $manifest){$previous=Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json}
foreach($item in @(@($destination,'ShaderPath','ShaderSHA256'),@($config,'ConfigPath','ConfigSHA256'))){
    if(Test-Path -LiteralPath $item[0]){
        if(!$previous -or $previous.($item[1]) -ne $item[0] -or (Get-FileHash -LiteralPath $item[0]).Hash -ne $previous.($item[2])){
            throw "Existing unowned or changed file: $($item[0])"
        }
    }
}
$installed=Join-Path $gameRoot 'dx11_diagnostic.addon32'
$addonManifest=Join-Path $projectRoot 'local/deployment.json'
$old=Get-Content -LiteralPath $addonManifest -Raw | ConvertFrom-Json
if($old.InstalledFile -ne $installed -or (Get-FileHash -LiteralPath $installed).Hash -ne $old.SHA256){throw 'Installed addon differs from ownership record.'}
$ini=Join-Path $gameRoot 'ReShade.ini'
$iniHash=(Get-FileHash -LiteralPath $ini).Hash
$text=[IO.File]::ReadAllText($ini)
$general=[regex]::Match($text,'(?ms)^\[GENERAL\]\r?\n(?<body>.*?)(?=^\[|\z)')
if(!$general.Success){throw 'Missing ReShade GENERAL section.'}
$body=$general.Groups['body'].Value
$definitions=[regex]::Match($body,'(?m)^PreprocessorDefinitions=(?<value>[^\r\n]*)')
$values=if($definitions.Success){$definitions.Groups['value'].Value}else{''}
foreach($entry in @('MODEL=0','STYLE=0','INTENSITY=1.0','STRUCTURE=1.0','TONE=0.0','SKIN=1.0','AUTO_SKIN=1')){
    $key='DX11_NR_'+$entry.Split('=')[0]
    if($values -notmatch ('(^|,)\s*'+[regex]::Escape($key)+'=')){
        if($values.Length){$values+=','}
        $values+='DX11_NR_'+$entry
    }
}
if($definitions.Success){$body=$body.Remove($definitions.Index,$definitions.Length).Insert($definitions.Index,'PreprocessorDefinitions='+$values)}
else{$body="PreprocessorDefinitions=$values`r`n"+$body}
$edited=$text.Remove($general.Groups['body'].Index,$general.Groups['body'].Length).Insert($general.Groups['body'].Index,$body)
if($ValidateOnly){Write-Output 'Neural trial preflight passed; no game files changed.';return}
$backup=Join-Path $projectRoot ('local/neural-install-backup-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach($file in @($ini,$destination,$config,$manifest,(Join-Path $gameRoot 'ReShade.log'))){
    if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $backup}
}
if((Get-FileHash -LiteralPath $ini).Hash -ne $iniHash){throw 'ReShade settings changed during installation.'}
& (Join-Path $PSScriptRoot 'install-diagnostic.ps1') -GameDirectory $gameRoot -Update
$cache=Join-Path $projectRoot 'local/live-neural'
[IO.File]::WriteAllText($config,"$helper`n$runtime`n$cache`n",[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $shader -Destination $destination -Force
if((Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath $shader).Hash){throw 'Neural control copy failed verification.'}
if($edited -ne $text){[IO.File]::WriteAllText($ini,$edited,[Text.UTF8Encoding]::new($false))}
[ordered]@{
    IniPath=$ini;IniSHA256=(Get-FileHash -LiteralPath $ini).Hash
    ConfigPath=$config;ConfigSHA256=(Get-FileHash -LiteralPath $config).Hash
    ShaderPath=$destination;ShaderSHA256=(Get-FileHash -LiteralPath $destination).Hash
    HelperPath=$helper;HelperSHA256=(Get-FileHash -LiteralPath $helper).Hash
    RuntimePath=$dll;RuntimeSHA256=(Get-FileHash -LiteralPath $dll).Hash
    Backup=$backup;InstalledAt=(Get-Date).ToString('o')
    Default='New ReShade technique initially unchecked; subsequent choice follows user preset'
} | ConvertTo-Json | Set-Content -LiteralPath $manifest
Write-Output 'Installed experimental neural checkbox and helper configuration. Existing DLAA helper/settings preserved. No NVIDIA binaries copied into game.'
Write-Output "Prelaunch log/config backup: $backup"
