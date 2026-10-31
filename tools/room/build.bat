@echo off
rem Builds room_server.exe (a multiplayer room server) with clang-cl and ENet from deps/enet.
rem Usage: build.bat <output dir>   (default: this folder's build\)
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%build"
call "%HERE%..\vcenv.bat" || exit /b 1
set "ENET=%HERE%..\..\..\deps\enet"
if not exist "%ENET%\host.c" (echo room: no ENet at %ENET% & exit /b 1)
if not exist "%OUT%" mkdir "%OUT%"
set "ZSTD=%HERE%..\..\..\deps\zstd\lib"
if not exist "%ZSTD%\zstd.h" (echo room: no zstd at %ZSTD% & exit /b 1)
"%NX_CC%" /nologo /O2 /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /DZSTD_DISABLE_ASM /I"%ENET%\include" /I"%ZSTD%" ^
  "%HERE%room_server.c" "%ENET%\callbacks.c" "%ENET%\compress.c" "%ENET%\host.c" "%ENET%\list.c" ^
  "%ENET%\packet.c" "%ENET%\peer.c" "%ENET%\protocol.c" "%ENET%\win32.c" ^
  "%ZSTD%\common\*.c" "%ZSTD%\decompress\*.c" ^
  /Fo"%OUT%\\" /Fe"%OUT%\room_server.exe" /link ws2_32.lib winmm.lib || exit /b 1
echo built %OUT%\room_server.exe
