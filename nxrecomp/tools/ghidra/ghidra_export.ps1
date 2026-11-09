# Analyse a game's modules with Ghidra (headless) and write <analysis>/<module>_ida.json for the lifter, in the
# format of ida_export.py. Run inside the game's repository (game.json in it or a parent, or NX_GAME set).
# The Ghidra project is kept in <analysis>/ghidra and reused: a module already in it is exported again without
# being analysed again.
# Usage: ghidra_export.ps1 [-Modules main,sdk] [-Seeds] [-Ghidra <install dir, default $env:GHIDRA_HOME>] [-MaxMem 12G]
#   -Seeds: also make functions at nxmod.py's code seeds (exports, RELATIVE relocations, init_array)
param([string[]]$Modules, [switch]$Seeds, [string]$Ghidra = $env:GHIDRA_HOME,
      [string]$MaxMem = '12G')
$ErrorActionPreference = 'Stop'
if (-not $Ghidra -or -not (Test-Path "$Ghidra\support\analyzeHeadless.bat")) {
    throw 'no Ghidra: set GHIDRA_HOME to its install folder (the one with support\analyzeHeadless.bat) or pass -Ghidra'
}
$cfgPath = if ($env:NX_GAME) { $env:NX_GAME } else {
    $d = (Get-Location).Path
    while ($d -and -not (Test-Path "$d\game.json")) { $d = Split-Path -Parent $d }
    if (-not $d) { throw 'no game.json: run inside a game repository or set NX_GAME' }
    "$d\game.json"
}
$gameDir = Split-Path -Parent (Resolve-Path $cfgPath)
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$elfDir = [IO.Path]::GetFullPath("$gameDir\$($cfg.dump)\elf")
$analysis = [IO.Path]::GetFullPath("$gameDir\$($cfg.analysis)")
if (-not $Modules) { $Modules = $cfg.modules | ForEach-Object { $_[0] } }
$proj = "$analysis\ghidra"
New-Item -ItemType Directory -Force $proj | Out-Null
$env:GHIDRA_HEADLESS_MAXMEM = $MaxMem
$headless = "$Ghidra\support\analyzeHeadless.bat"
foreach ($m in $Modules) {
    $elf = "$elfDir\$m.elf"
    $out = "$analysis\${m}_ida.json"
    $post = @('-postScript', 'GhidraExport.java', $out)
    if ($Seeds) {
        $seedFile = "$analysis\${m}_seeds.json"
        python "$PSScriptRoot\..\nxmod.py" seeds $elf $seedFile
        $post += $seedFile
    }
    $t0 = Get-Date
    if (Test-Path "$proj\$m.rep\idata\~index.dat") {
        # analysed before: export only
        & $headless $proj $m -process "$m.elf" -noanalysis -scriptPath $PSScriptRoot @post -log "$analysis\${m}_ghidra.log"
    } else {
        & $headless $proj $m -import $elf -loader ElfLoader -loader-imagebase 0 -max-cpu 0 `
            -scriptPath $PSScriptRoot -preScript GhidraOptions.java @post -log "$analysis\${m}_ghidra.log"
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) { throw "Ghidra failed on $m (see $analysis\${m}_ghidra.log)" }
    '{0}: {1:N0} s' -f $m, ((Get-Date) - $t0).TotalSeconds
}
