# The workspace's front door: start, build or package anything from here. .\start.ps1 alone lists it all.
# Each command wraps the project's own script, which still works on its own.
# Plain $args (no param block): options for the projects' own scripts (-NoBuild, -Log 2, ...) pass through as typed.
$What = if ($args.Count) { [string]$args[0] } else { '' }
$Target = if ($args.Count -gt 1) { [string]$args[1] } else { '' }
$Rest = if ($args.Count -gt 2) { [string[]]$args[2..($args.Count - 1)] } else { @() }
$ErrorActionPreference = 'Stop'
$ws = $PSScriptRoot

$games = @{
    violet = @{ repo = "$ws\pkmn-violet\violetrecomp"; id = '01008F6008C5E000'; title = 'Pokemon Violet' }
    mk8    = @{ repo = "$ws\mk8dx\mk8recomp";             id = '0100152000022000'; title = 'Mario Kart 8 Deluxe' }
}

function Show-Help {
    @'
.\start.ps1 <command> ...

  Play
    violet [-NoBuild] [-Room host[:port[:pw]]] [-Nick name]   Pokemon Violet (builds first unless -NoBuild)
    mk8    [-NoBuild] [-Room host[:port[:pw]]] [-Nick name]   Mario Kart 8 Deluxe
           (no -Room: starts offline, rooms are in the game's menu; other options pass through: -Log 2, ...)

  Local play
    room   [violet|mk8] [-Name "..."] [-Password pw] [-Port 24872] [-Beacon]
                                   host a room (builds it the first time); -Beacon: the beacon too, in this
                                   console (the room runs hidden, logging to the capture folder; Ctrl+C stops both)
    beacon [ldnbeacon options]     a real Switch into the room on 127.0.0.1:24872 through the ESP32-S3;
                                   log and pcap go to ldn-beacon\captures\<time>\

  Build
    build violet|mk8|room|beacon   build one project
    package violet|mk8 <out dir>   a folder for players (nxrecomp\tools\package.ps1)

  Where things are: README.md (setup, layout), each project's CLAUDE.md (start here, state), NOTES.md (history).
'@
}

function Get-Opt([string[]]$a, [string]$name, $default = $null) {
    if (-not $a) { return $default }                    # no options at all: PowerShell passes $null, not @()
    $i = [Array]::IndexOf($a, $name)
    if ($i -ge 0 -and $i + 1 -lt $a.Count) { return $a[$i + 1] }
    return $default
}
function Remove-Opt([string[]]$a, [string[]]$withValue, [string[]]$flags = @()) {
    $out = @()
    for ($i = 0; $i -lt $a.Count; $i++) {
        if ($withValue -contains $a[$i]) { $i++; continue }
        if ($flags -contains $a[$i]) { continue }
        $out += $a[$i]
    }
    return , $out
}

# "-Log 2 -NoBuild" -> @{Log = '2'; NoBuild = $true}: a script's named parameters only bind from a hashtable splat
function ConvertTo-Splat([string[]]$a) {
    $h = @{}
    for ($i = 0; $i -lt $a.Count; $i++) {
        if ($a[$i] -notlike '-*') { throw "unexpected '$($a[$i])' (options are -Name value or -Switch)" }
        $k = $a[$i].TrimStart('-')
        if ($i + 1 -lt $a.Count -and $a[$i + 1] -notlike '-*') { $h[$k] = $a[$i + 1]; $i++ } else { $h[$k] = $true }
    }
    return $h
}

function Start-Game([string]$name, [string[]]$a) {
    $g = $games[$name]
    $room = Get-Opt $a '-Room'
    $nick = Get-Opt $a '-Nick'
    $pass = ConvertTo-Splat (Remove-Opt $a @('-Room', '-Nick'))
    if ($room) { $env:NX_ROOM = $room } else { Remove-Item Env:NX_ROOM -ErrorAction SilentlyContinue }
    if ($nick) { $env:NX_NICK = $nick }
    Push-Location $g.repo
    try { & "$($g.repo)\run.ps1" @pass } finally { Pop-Location }
}

function Build-Project([string]$name) {
    switch ($name) {
        { $games.ContainsKey($_) } { cmd /c "`"$($games[$name].repo)\build.bat`""; break }
        'room'   { cmd /c "`"$ws\room\build.bat`""; break }
        'beacon' { cmd /c "`"$ws\ldn-beacon\build.bat`""; break }
        default  { throw "build what? violet, mk8, room or beacon" }
    }
    if ($LASTEXITCODE) { throw "the $name build failed" }
}

# the beacon (ldn-beacon\build\ldnbeacon.exe), into the room at 127.0.0.1:<port> unless --room is given
function New-CaptureFolder {
    $run = "$ws\ldn-beacon\captures\$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Force $run | Out-Null
    return $run
}
function Start-Beacon([string[]]$a, [string]$port = '24872', [string]$run = '') {
    if (-not $a) { $a = @() }
    $exe = "$ws\ldn-beacon\build\ldnbeacon.exe"
    if (-not (Test-Path $exe)) { Build-Project 'beacon' }
    if (-not $run) { $run = New-CaptureFolder }
    if ($a -notcontains '--room') { $a = @('--room', "127.0.0.1:$port") + $a }
    if ($a -notcontains '--pcap') { $a += @('--pcap', "$run\beacon.pcap") }
    "log: $run\beacon.log"
    Push-Location "$ws\ldn-beacon"
    try { & $exe @a 2>&1 | Tee-Object -FilePath "$run\beacon.log" } finally { Pop-Location }
}

switch ($What) {
    { $_ -in 'violet', 'mk8' } { Start-Game $What (@($Target) + $Rest | Where-Object { $_ }) }

    'room' {
        $a = @($Rest)
        $game = 'violet'
        if ($Target -and $games.ContainsKey($Target)) { $game = $Target } elseif ($Target) { $a = @($Target) + $a }
        $exe = "$ws\room\build\room_server.exe"
        if (-not (Test-Path $exe)) { Build-Project 'room' }
        $g = $games[$game]
        $args2 = @('--port', (Get-Opt $a '-Port' '24872'), '--name', (Get-Opt $a '-Name' "$($g.title) room"),
                   '--game-id', $g.id, '--game', $g.title)
        $pw = Get-Opt $a '-Password'
        if ($pw) { $args2 += @('--password', $pw) }
        if ($a -notcontains '-Beacon') { & $exe @args2; break }
        # the room hidden in this console (its output in the capture folder), the beacon in the foreground;
        # stopping the beacon (Ctrl+C) stops the room too
        if (-not (Test-Path "$ws\ldn-beacon\build\ldnbeacon.exe")) { Build-Project 'beacon' }
        $run = New-CaptureFolder
        $quoted = $args2 | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
        $proc = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory "$ws\room\build" -NoNewWindow -PassThru `
                              -RedirectStandardOutput "$run\room.log" -RedirectStandardError "$run\room-err.log"
        Start-Sleep -Milliseconds 800
        if ($proc.HasExited) { throw "the room server stopped at once (exit $($proc.ExitCode); $run\room-err.log): is the port in use?" }
        "room: $($args2[3]) on UDP $($args2[1]), log $run\room.log"
        try { Start-Beacon @() $args2[1] $run }
        finally { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -Confirm:$false } }
    }

    'beacon' { Start-Beacon (@(@($Target) + $Rest | Where-Object { $_ })) }

    'build' { Build-Project $Target }

    'package' {
        if (-not $games.ContainsKey($Target) -or -not $Rest) { throw 'package violet|mk8 <out dir>' }
        $out = [IO.Path]::GetFullPath($Rest[0])
        Push-Location $games[$Target].repo
        $more = ConvertTo-Splat @($Rest | Select-Object -Skip 1)
        try { & "$ws\nxrecomp\tools\package.ps1" -Out $out @more } finally { Pop-Location }
    }

    default { Show-Help }
}
