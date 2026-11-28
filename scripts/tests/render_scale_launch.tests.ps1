# Windows integration test. A disposable child reads preferences before creating its first frame.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../render_scale_preference.ps1')
. (Join-Path $PSScriptRoot '../../tools/analysis/native_scale_frames.ps1')
$runner = Join-Path $PSScriptRoot '../launch_render_scale_trial.ps1'
$dir = Join-Path ([IO.Path]::GetTempPath()) ('na-scale-trial-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($dir)
$pref = Join-Path $dir 'prefs.txt'
$ini = Join-Path $dir 'NeuralAliasHost.ini'
$profile = Join-Path $dir 'trial.json'
$exe = Join-Path $dir 'ScaleTrialFixture.exe'
$regular = '"Graphics"={ "unused"={} }' + [Environment]::NewLine + '  "render_scale"={ version=0 value=1.000000 }' + [Environment]::NewLine + '# keep this'
$hostOriginal = "[one]" + [Environment]::NewLine + "enabled=1" + [Environment]::NewLine + "[j2]" + [Environment]::NewLine + "enabled=1"
function Require($condition, $reason) { if (!$condition) { throw $reason } }
function Bytes($path) { [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) }
try {
    $source = @'
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;
class ScaleTrialFixture {
    static int Main(string[] args) {
        string p = "prefs.txt";
        string prefs = File.ReadAllText(p);
        bool changed = prefs.Contains("value=0.700000");
        bool disabled = Regex.IsMatch(File.ReadAllText("NeuralAliasHost.ini"), @"\[j2\]\r?\nenabled=0\b");
        string size = changed && disabled && args[0] != "ignore" ? "2408x1008" : "3440x1440";
        string[] pieces = {
          "[d #024 fmt=10 1720x720 no-depth n=1 tested=0 ]",
          "[d #043 fmt=29 " + size + " z#345=scene(" + size + ",s=1) n=89 tested=85 PICTURE]",
          "[d #122 fmt=87 3440x1440 no-depth n=4 tested=0 screen]"
        };
        string trace = "native11_flow frame=101 steps=3 dropped=0 part=1: " + pieces[0] + " " + pieces[1] +
           Environment.NewLine + "native11_flow frame=101 steps=3 dropped=0 part=2: " + pieces[2];
        File.WriteAllText("status-" + Process.GetCurrentProcess().Id + ".log", trace);
        if (args[0] == "edit") File.AppendAllText(p, Environment.NewLine + "# new unrelated preference");
        if (args[0] == "player") File.WriteAllText(p, prefs.Replace("value=0.700000", "value=0.850000"));
        return 0;
    }
}
'@
    $cs = Join-Path $dir 'fixture.cs'
    [IO.File]::WriteAllText($cs,$source)
    $compiler = Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $compiler /nologo /target:exe "/out:$exe" $cs
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
    $field = @{ Format='block'; Key='render_scale' }
    foreach ($case in 'apply','ignore','edit','player') {
        [IO.File]::WriteAllText($pref,$regular,[Text.Encoding]::UTF8)
        [IO.File]::WriteAllText($ini,$hostOriginal,[Text.Encoding]::Unicode)
        $prefBefore = Bytes $pref; $iniBefore = Bytes $ini
        $p = @{Preference=$pref; Field=$field; Executable=$exe; Arguments=@($case); LogFolder=$dir; Width=3440; Height=1440; Scale=0.70}
        [IO.File]::WriteAllText($profile,(ConvertTo-Json $p -Depth 4))
        & $runner -Profile $profile | Out-Null
        $result = [IO.File]::ReadAllText((Join-Path $dir 'last-scale-result.txt'))
        Require ($result.StartsWith($(if ($case -eq 'ignore') {'REQUEST NOT OBSERVED:'} else {'OBSERVED:'}))) "Incorrect geometry verdict for $case"
        Require ((Bytes $ini) -ceq $iniBefore) 'Recorder not restored byte-for-byte'
        if ($case -in 'apply','ignore') { Require ((Bytes $pref) -ceq $prefBefore) 'Preference was not restored byte-for-byte' }
        if ($case -eq 'edit') { Require ([IO.File]::ReadAllText($pref).Contains('value=1.000000') -and [IO.File]::ReadAllText($pref).Contains('# new unrelated preference')) 'Other edits lost' }
        if ($case -eq 'player') { Require ([IO.File]::ReadAllText($pref).Contains('value=0.850000')) 'New player choice lost' }
        Require (!(Test-Path (Join-Path $dir 'scale-restore.json'))) 'Active journal after restoration'
    }
    # Simulated interruption after the first file update, before the host config changes.
    [IO.File]::WriteAllText($pref,$regular)
    $edits = @((New-ScaleEdit $pref $field '0.700000'),(New-ScaleEdit $ini @{Format='ini'; Section='j2'; Key='enabled'} '0'))
    [IO.File]::WriteAllText((Join-Path $dir 'scale-restore.json'),(ConvertTo-Json -InputObject $edits -Depth 4))
    Write-ScaleField $edits[0] $false
    & $runner -Profile $profile -RestoreOnly
    Require ([IO.File]::ReadAllText($pref) -ceq $regular) 'Interrupted first write did not recover'
    # A shadow, missing fragment, dropped records or a screen draw before the world cannot pass.
    $world = '[d #a fmt=29 2408x1008 z#b=scene(2408x1008,s=1) n=15 tested=15 PICTURE]'
    $screen = '[d #c fmt=87 3440x1440 no-depth n=1 tested=0 screen]'
    $bad = @("native11_flow frame=1 steps=2 dropped=0 part=1: $screen $world",
        "native11_flow frame=2 steps=3 dropped=0 part=1: $world $screen",
        "native11_flow frame=3 steps=2 dropped=1 part=1: $world $screen",
        "native11_flow frame=4 steps=2 dropped=0 part=2: $world $screen",
        "native11_flow frame=5 steps=2 dropped=0 part=1: $($world.Replace('=scene','')) $screen")
    $log = Join-Path $dir 'negatives.log'
    [IO.File]::WriteAllLines($log,$bad)
    Require (@(Get-ScaleFrames $log).Count -eq 0) 'False positive frame'
    Write-Output 'PASS: first-frame scale, ignored request, independent preferences, player override, interrupted journal, five invalid GPU traces.'
} finally {
    $resolved = [IO.Path]::GetFullPath($dir)
    if ([IO.Path]::GetDirectoryName($resolved).TrimEnd('\') -eq [IO.Path]::GetTempPath().TrimEnd('\') -and [IO.Path]::GetFileName($resolved) -match '^na-scale-trial-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
