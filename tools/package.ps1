# A folder for players (run inside a game's repo): the packaged executable (NX_PACKAGE: the game read from the
# player's own NSPs and keys in game\, data under %LOCALAPPDATA%\<title>\), its DLLs, the shaders (translated
# beforehand in shaders.pak, or the translator: an embeddable Python, translate.py, glslang) and the room server.
# Usage: package.ps1 -Out <dir> [-ShaderPack <shaders.pak> | -PythonZip <python-3.x-embed-amd64.zip>] [-NoBuild]
#   (neither)    the default: every shader in the game's shader folder (<build>\shaders) translated now
#                (translate.py --spv) and packed (nxshpack: tools/shader/pack.c) into <build>\shaders.pak, which goes
#                next to the exe; no translator in the package. Built from the game's files, as the exe's lifted code is
#   -ShaderPack  a pack made before (the same, without translating again)
#   -PythonZip   Python's official "Windows embeddable package (64-bit)" (python.org), unpacked into translator\python:
#                shaders translated on the player's PC as the game first uses them; glslang comes from
#                %VULKAN_SDK%\Bin\glslangValidator.exe
param([Parameter(Mandatory)] [string]$Out, [string]$ShaderPack, [string]$PythonZip, [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
if ($ShaderPack -and $PythonZip) { throw 'give -ShaderPack or -PythonZip, not both' }
if ($ShaderPack -and -not (Test-Path $ShaderPack)) { throw "no $ShaderPack" }
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
if (-not $ShaderPack -and -not $PythonZip) {
    # every shader for the translator version in this tree, then the pack (the runtime uses a pack only when its
    # translator version matches, gpu.c pak_open)
    Push-Location $d
    try {
        python "$tools\shader\translate.py" --spv
        if ($LASTEXITCODE) { throw 'translate.py --spv failed' }
        $ver = python -c "import sys; sys.path.insert(0, r'$tools\shader'); import translate; print(translate.translator_version())"
        if ($LASTEXITCODE -or -not $ver) { throw 'no translator version' }
    } finally { Pop-Location }
    $spv = "$build\shaders\spv\$($ver.Trim())"
    if (-not (Test-Path $spv)) { throw "no $spv" }
    if (-not $NoBuild -or -not (Test-Path "$build\cmake\nxshpack.exe")) {
        cmd /c "`"$d\build.bat`" --target nxshpack"
        if ($LASTEXITCODE) { throw 'the nxshpack build failed' }
    }
    $ShaderPack = "$build\shaders.pak"
    & "$build\cmake\nxshpack.exe" $spv $ShaderPack
    if ($LASTEXITCODE) { throw 'nxshpack failed' }
}
if (-not $ShaderPack -and (-not $env:VULKAN_SDK -or -not (Test-Path "$env:VULKAN_SDK\Bin\glslangValidator.exe"))) {
    throw 'no %VULKAN_SDK%\Bin\glslangValidator.exe'
}

New-Item -ItemType Directory -Force $Out, "$Out\game", "$Out\game\firmware", "$Out\room" | Out-Null
Copy-Item $exe.FullName, "$pkg\*.dll" $Out -Force
# the Visual C++ runtime next to the exe (app-local, as Microsoft allows), so players need no redistributable;
# the UCRT (api-ms-win-crt-*) is part of Windows 10 and 11
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath }
$crt = if ($vs) { Get-ChildItem "$vs\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ChildItem "$($_.FullName)\x64" -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue } |
    Sort-Object FullName | Select-Object -Last 1 }
if (-not $crt) { throw "no Visual C++ redistributable folder (VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT) in Visual Studio" }
$vcrt = 'vcruntime140.dll', 'vcruntime140_1.dll' | ForEach-Object { "$($crt.FullName)\$_" }
if ($ShaderPack) {
    # the pack replaces the translator: without translate.py the runtime takes shaders from the pack only (gpu.c)
    if (Test-Path "$Out\translator") { Remove-Item -Recurse -Force -Confirm:$false "$Out\translator" }
    Copy-Item $ShaderPack "$Out\shaders.pak" -Force
} else {
    if (Test-Path "$Out\shaders.pak") { Remove-Item -Force -Confirm:$false "$Out\shaders.pak" }
    New-Item -ItemType Directory -Force "$Out\translator\Bin" | Out-Null
    Copy-Item "$tools\shader\translate.py", "$tools\shader\maxwell.py" "$Out\translator" -Force
    Copy-Item "$env:VULKAN_SDK\Bin\glslangValidator.exe" "$Out\translator\Bin" -Force
    if (Test-Path "$Out\translator\python") { Remove-Item -Recurse -Force -Confirm:$false "$Out\translator\python" }
    Expand-Archive $PythonZip "$Out\translator\python"
    # the embeddable Python's search path is its ._pth file: add translator\ (translate.py imports maxwell.py)
    $pth = Get-ChildItem "$Out\translator\python" -Filter 'python*._pth' | Select-Object -First 1
    Add-Content $pth.FullName '..'
}
cmd /c "`"$tools\room\build.bat`" `"$Out\room`""
if ($LASTEXITCODE) { throw 'the room server build failed' }
Remove-Item "$Out\room\*.obj" -ErrorAction SilentlyContinue
Copy-Item $vcrt $Out -Force
Copy-Item $vcrt "$Out\room" -Force

$title = $cfg.title
$shaders = if ($ShaderPack) { @"
The game's shaders come translated for PC already (shaders.pak, next to the exe). The first time an area is
shown it can stutter briefly while your graphics driver compiles what it needs; later starts reuse that work.
"@ } else { @"
The first start shows a black window for about a minute: the game's shaders are translated for your PC as they are
first needed (thousands at the title screen). Later starts reuse them. New areas can stutter the first time too.
The translator folder is needed while playing: new shaders are translated as the game first uses them.
"@ }
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

$shaders
Nothing is extracted or installed: the game is read from your NSPs as it runs. Saves, caches, settings and the log
are in %LOCALAPPDATA%\$title\ (save\, cache\, settings.ini, log.txt). Deleting cache\ is safe; it is rebuilt (the
first minutes of play stutter again).

Controls: WASD moves, the mouse looks (click in the window to capture it), Z = A, X = B, C = X, V = Y, Q = L,
R = R, 1 = ZL, 3 = ZR, Enter = +, Backspace = -, arrow keys = D-pad. A gamepad works too.
Esc opens the options (mouse sensitivity, 30 or 60 fps, FPS counter, render scale 1-4x: 2x draws at 4K and is much
smoother on a 1440p or 4K screen if your graphics card keeps up); Esc again closes them.
F3 shows the frame rate. F12 captures the next frame into %LOCALAPPDATA%\$title\dumps: if something looks wrong,
press it and send that folder's newest files with log.txt.

Multiplayer (local play, Union Circle, Tera raids) goes through a room server that relays the game's traffic: one
player runs room\room_server.exe (UDP port 24872), everyone else picks that address in the room browser when the
game starts. On one network nothing needs setting up. Over the internet only the server's machine must be
reachable: forward UDP 24872, use a UDP tunnel (e.g. playit.gg) or a VPN such as Tailscale. Players need no
forwarding.

Needs: Windows 10 or 11 (64-bit) and a graphics driver with Vulkan 1.3 (any recent NVIDIA, AMD or Intel driver).
Nothing else needs installing.
"@ | Set-Content "$Out\README.txt"
"Put your NSPs, prod.keys and title.keys here. Firmware .nca files go in the firmware folder." | Set-Content "$Out\game\PUT YOUR FILES HERE.txt"
$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"{0}: {1:N0} MB" -f $Out, $size
