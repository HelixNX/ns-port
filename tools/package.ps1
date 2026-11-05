# A folder for players (run inside a game's repo): the packaged executable (NX_PACKAGE: the game read from the
# player's own NSPs and keys in game\, data under %LOCALAPPDATA%\<title>\), its DLLs, the shader translator (an
# embeddable Python, translate.py, glslang) and the room server. Nothing from the game goes in it.
# Usage: package.ps1 -Out <dir> -PythonZip <python-3.x-embed-amd64.zip> [-NoBuild]
#   -PythonZip  Python's official "Windows embeddable package (64-bit)" (python.org), unpacked into translator\python
#   glslang comes from %VULKAN_SDK%\Bin\glslangValidator.exe
param([Parameter(Mandatory)] [string]$Out, [Parameter(Mandatory)] [string]$PythonZip, [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$d = (Get-Location).Path
while ($d -and -not (Test-Path "$d\game.json")) { $d = Split-Path -Parent $d }
if (-not $d) { throw 'run inside a game repository (no game.json found)' }
$cfg = Get-Content "$d\game.json" -Raw | ConvertFrom-Json
$build = [IO.Path]::GetFullPath("$d\$($cfg.build)")
$pkg = "$build\cmake\package"
if (-not $NoBuild) {
    cmd /c "`"$d\build.bat`" --target $($cfg.name)_pkg"
    if ($LASTEXITCODE) { throw 'the package build failed' }
}
$exe = Get-ChildItem $pkg -Filter *.exe | Select-Object -First 1
if (-not $exe) { throw "no packaged executable in $pkg" }
if (-not $env:VULKAN_SDK -or -not (Test-Path "$env:VULKAN_SDK\Bin\glslangValidator.exe")) { throw 'no %VULKAN_SDK%\Bin\glslangValidator.exe' }

New-Item -ItemType Directory -Force $Out, "$Out\game", "$Out\game\firmware", "$Out\translator\Bin", "$Out\room" | Out-Null
Copy-Item $exe.FullName, "$pkg\*.dll" $Out -Force
Copy-Item "$tools\shader\translate.py", "$tools\shader\maxwell.py" "$Out\translator" -Force
Copy-Item "$env:VULKAN_SDK\Bin\glslangValidator.exe" "$Out\translator\Bin" -Force
if (Test-Path "$Out\translator\python") { Remove-Item -Recurse -Force -Confirm:$false "$Out\translator\python" }
Expand-Archive $PythonZip "$Out\translator\python"
# the embeddable Python's search path is its ._pth file: add translator\ (translate.py imports maxwell.py)
$pth = Get-ChildItem "$Out\translator\python" -Filter 'python*._pth' | Select-Object -First 1
Add-Content $pth.FullName '..'
cmd /c "`"$tools\room\build.bat`" `"$Out\room`""
if ($LASTEXITCODE) { throw 'the room server build failed' }
Remove-Item "$Out\room\*.obj" -ErrorAction SilentlyContinue

$title = $cfg.title
@"
$title (Windows)

What you need (your own, from your own Switch and game cartridge or eShop copy):
  - the base game NSP and the 4.0.0 update NSP (the DLC NSP too, if you own it)
  - prod.keys (and title.keys if you have one)
  - firmware: the NCA files of a firmware dump (local multiplayer needs one of them)

Setup:
  1. Put the NSPs, prod.keys and title.keys in the game folder next to "$title.exe".
  2. Put the firmware's .nca files in game\firmware.
  3. Run "$title.exe".

The first start shows a black window for about a minute: the game's shaders are translated for your PC as they are
first needed (thousands at the title screen). Later starts reuse them. New areas can stutter the first time too.

Nothing is extracted or installed: the game is read from your NSPs as it runs. Saves, caches and the log are in
%LOCALAPPDATA%\$title\ (save\, cache\, log.txt). Deleting cache\ is safe; it is rebuilt (the first minutes of
play stutter again while shaders are translated). The translator folder is needed while playing: new shaders
are translated as the game first uses them.

Keys: F3 shows the frame rate. F12 captures the next frame into %LOCALAPPDATA%\$title\dumps: if something
looks wrong, press it and send that folder's newest files with log.txt.

Multiplayer (local play, Union Circle, Tera raids) goes through a room server: one player runs room\room_server.exe
(port 24872, UDP; forward it or use a VPN such as Tailscale), everyone else picks that address in the room
browser when the game starts.
"@ | Set-Content "$Out\README.txt"
"Put your NSPs, prod.keys and title.keys here. Firmware .nca files go in the firmware folder." | Set-Content "$Out\game\PUT YOUR FILES HERE.txt"
$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"{0}: {1:N0} MB" -f $Out, $size
