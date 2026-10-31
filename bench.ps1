# Stand-still benchmark (CLAUDE.md, Next): one run with extra NX_* switches, then the frame time and GPU busy
# (NX_GPU_TIMELINE) averaged over the windows after the load. Logs to build/bench_<name>.log.
# Usage: .\bench.ps1 <name> [NAME=value ...]          one run (~140 s; keep hands off the window)
#        .\bench.ps1 <name> -Summary                   the averages of an earlier run's log
#        .\bench.ps1 <name> -Rgp 19000 [NAME=value ...] an RGP capture of that frame into build/<name>.rgp
#                                                       (Adrenalin 26.10.2 and the RDTS in .tools/rdts)
param([string]$Name, [switch]$Summary, [int]$Rgp = 0, [Parameter(ValueFromRemainingArguments)] [string[]]$Env = @())
$b = Join-Path (Split-Path -Parent $PSScriptRoot) 'build'
$log = "$b\bench_$Name.log"
if (-not $Summary) {
    if (Test-Path "$b\save-idle-run") { Remove-Item -Recurse -Force -Confirm:$false "$b\save-idle-run" }
    Copy-Item -Recurse "$b\save" "$b\save-idle-run"
    $set = @{ NX_NOPACE = '1'; NX_TIME = '1792494000'; NX_INPUT = '300-900/60:A'; NX_GPU_TIMELINE = '1' }
    foreach ($e in $Env) { $k, $v = $e -split '=', 2; $set[$k] = $v }
    $cli = $null
    $rgpFile = "$b\$Name.rgp"
    if ($Rgp) {         # the Panel CLI must be waiting before the game starts; neither exits after the capture
        $tools = Get-ChildItem (Join-Path $PSScriptRoot '..\..\.tools\rdts') -Recurse -Filter RadeonDeveloperPanelCLI.exe |
            Select-Object -First 1
        if (-not $tools) { throw 'no RadeonDeveloperPanelCLI.exe under .tools\rdts' }
        if (Test-Path $rgpFile) { Remove-Item -Force $rgpFile }
        $cli = Start-Process -FilePath $tools.FullName -PassThru -NoNewWindow -RedirectStandardOutput "$b\rgp_$Name.out" `
            -ArgumentList @('-m', 'profiling', '-p', 'violetrecomp', "--rgp-auto-capture=frame:$Rgp",
                            '--rgp-sqtt-buffer-size', 'high', '-o', $rgpFile)
        Start-Sleep -Seconds 3
    }
    foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
    try {
        $p = Start-Process -FilePath "$b\cmake\violetrecomp.exe" -ArgumentList @('--log', '1', '--save', "$b\save-idle-run") `
            -RedirectStandardError $log -NoNewWindow -PassThru
        $t0 = Get-Date
        while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt ($Rgp ? 150 : 135)) {
            Start-Sleep -Seconds 2
            if ($Rgp -and (Test-Path $rgpFile)) { Start-Sleep -Seconds 5; break }   # a cut-off trace fails (-2)
        }
        if (-not $p.HasExited) { $p.Kill() }
    } finally {
        foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
        if ($cli -and -not $cli.HasExited) { Start-Sleep -Seconds 3; $cli.Kill() }
    }
    if ($Rgp) { if (Test-Path $rgpFile) { "{0}: {1:N1} MB" -f $rgpFile, ((Get-Item $rgpFile).Length / 1MB) } else { "no capture (see $b\rgp_$Name.out)" } }
}
$ft = @(Select-String -Path $log -Pattern 'nvn: frame time ([\d.]+) ms')
$first = 0         # the load's two hitch windows: the first window over 3 ms (title and load run ~1 ms) and the next
while ($first -lt $ft.Count -and [double]$ft[$first].Matches[0].Groups[1].Value -lt 3) { $first++ }
$use = @($ft | Select-Object -Skip ($first + 2))
if (-not $use.Count) { "${Name}: no windows after the load"; return }
$avg = ($use | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Average).Average
$tl = @(Select-String -Path $log -Pattern 'gpu timeline: per frame ([\d.]+) ms: GPU busy ([\d.]+)' |
    Select-Object -Last $use.Count)
$busy = ($tl | ForEach-Object { [double]$_.Matches[0].Groups[2].Value } | Measure-Object -Average).Average
"{0}: {1} windows, frame {2:N3} ms ({3:N1} fps), GPU busy {4:N3} ms ({5} timeline lines)" -f $Name, $use.Count, $avg,
    (1000 / $avg), $busy, $tl.Count
Select-String -Path $log -Pattern 'not in its format list|device lost|VK_ERROR|VUID' | Select-Object -First 10 |
    ForEach-Object { $_.Line }
