# Decompile functions of a game module with Ghidra (headless, read-only) from the project ghidra_export.ps1 made.
# Usage (inside the game's repo): decomp.ps1 <out file> <offset>... [-Module main] [-Ghidra <install dir>]
#   an offset is hex from the module's base; `<off>!` creates a function there if Ghidra has none (decompiles it
#   from that address), `x<off>` lists only the callers of the function containing it
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Mandatory, Position = 0)] [string]$Out, [Parameter(ValueFromRemainingArguments)] [string[]]$Addrs,
      [string]$Module = 'main', [string]$Ghidra = $env:GHIDRA_HOME)
if (-not $Ghidra -or -not (Test-Path "$Ghidra\support\analyzeHeadless.bat")) {
    throw 'no Ghidra: set GHIDRA_HOME to its install folder (the one with support\analyzeHeadless.bat) or pass -Ghidra'
}
$cfgPath = if ($env:NX_GAME) { $env:NX_GAME } else {
    $d = (Get-Location).Path
    while ($d -and -not (Test-Path "$d\game.json")) { $d = Split-Path -Parent $d }
    if (-not $d) { throw 'no game.json: run inside a game repository or set NX_GAME' }
    "$d\game.json"
}
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$analysis = [IO.Path]::GetFullPath("$(Split-Path -Parent (Resolve-Path $cfgPath))\$($cfg.analysis)")
$Out = [IO.Path]::GetFullPath($Out)
if (Test-Path $Out) { Remove-Item $Out }
& "$Ghidra\support\analyzeHeadless.bat" "$analysis\ghidra" $Module -process "$Module.elf" -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript Decomp.java $Out @Addrs *> "$Out.log"
if (Test-Path $Out) { Select-String -Path $Out -Pattern '^== |^callers' | ForEach-Object { $_.Line } } else { Get-Content "$Out.log" -Tail 20 }
