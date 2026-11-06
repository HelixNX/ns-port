# Build violetrecomp and run it, logging to build/run.log; prints the end of the log.
# Usage: .\run.ps1 [-Log 0-2] [-Timeout seconds] [-NoBuild] [-Tail n]
# No -Timeout (or 0): runs until the game is closed. With one, the game is killed after that many seconds
# (unattended test runs; a session cut off that way is not saved).
param([int]$Log = 1, [int]$Timeout = 0, [switch]$NoBuild, [int]$Tail = 40, [int]$DumpAfter = 0)
$root = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) {
    $out = cmd /c "$PSScriptRoot\build.bat --target violetrecomp 2>&1"
    if ($LASTEXITCODE -ne 0) { $out | Select-String -Pattern "error" | Select-Object -First 30; exit 1 }
}
$exe = "$root\build\cmake\violetrecomp.exe"
$args2 = @('--log', $Log); if ($DumpAfter) { $args2 += @('--dump-after', $DumpAfter) }
$p = Start-Process -FilePath $exe -ArgumentList $args2 -RedirectStandardError "$root\build\run.log" -NoNewWindow -PassThru
if ($Timeout -le 0) { $p.WaitForExit() }
elseif (-not $p.WaitForExit($Timeout * 1000)) { $p.Kill(); 'killed after timeout' }
'exit ' + $p.ExitCode
Get-Content "$root\build\run.log" -Tail $Tail
