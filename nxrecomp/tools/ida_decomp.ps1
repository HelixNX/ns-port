# Whole-module decompile with IDA (idat batch + Hex-Rays); the work is in ida_decomp.py.
#   ida_decomp.ps1 <module.elf> <out dir> [-Jobs 8] [-Seeds <module>_ida.json] [-Ida <IDA dir>] [-AnalyzeOnly]
# 1. no <module.elf>.i64 yet: auto-analysis (main: about 40 min), -Seeds adds the lifter's function entries.
# 2. -Jobs copies of the .i64 under <out dir>\work, one idat each, shard i writes <out dir>\<module>_<i>.c and .log.
# IDA is found from -Ida, IDADIR, or an "IDA*" folder in Program Files.
param(
    [Parameter(Mandatory)][string]$Elf,
    [Parameter(Mandatory)][string]$Out,
    [int]$Jobs = 8,
    [string]$Seeds,
    [string]$Ida = $env:IDADIR,
    [switch]$AnalyzeOnly
)
$ErrorActionPreference = 'Stop'
if (-not $Ida) {
    $Ida = Get-ChildItem $env:ProgramFiles -Directory -Filter 'IDA*' | Where-Object { Test-Path "$($_.FullName)\idat.exe" } |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
}
$idat = Join-Path $Ida 'idat.exe'
if (-not (Test-Path $idat)) { throw 'no idat.exe: pass -Ida <IDA dir> or set IDADIR' }
$Elf = (Resolve-Path $Elf).Path
$py = Join-Path $PSScriptRoot 'ida_decomp.py'
$env:TVHEADLESS = '1'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$db = "$Elf.i64"
$name = [IO.Path]::GetFileName($Elf)

if (-not (Test-Path $db)) {
    $arg = "analyze"
    if ($Seeds) { $arg += ' "' + (Resolve-Path $Seeds).Path + '"' }
    & $idat -A -c "-L$Out\$name.analyze.log" "-S`"$py`" $arg" $Elf | Out-Null
    if (-not (Test-Path $db)) { throw "analysis left no $db (see $Out\$name.analyze.log)" }
}
if ($AnalyzeOnly) { return }

$work = Join-Path $Out 'work'
$procs = foreach ($i in 0..($Jobs - 1)) {
    $dir = Join-Path $work $i
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $db "$dir\$name.i64" -Force
    Start-Process $idat -WindowStyle Hidden -PassThru -ArgumentList @(
        '-A', "`"-L$dir\idat.log`"", "`"-S\`"$py\`" decomp \`"$Out\`" $i $Jobs`"", "`"$dir\$name.i64`"")
}
$procs | Wait-Process
$bad = @($procs | Where-Object ExitCode -ne 0)
if ($bad.Count) { throw "$($bad.Count) of $Jobs idat runs failed (logs in $work\<i>\idat.log)" }
Remove-Item $work -Recurse -Force
Get-ChildItem $Out -Filter '*_*.log' | ForEach-Object { '{0}: {1}' -f $_.Name, (Get-Content $_.FullName -Tail 1) }
