@echo off
rem Builds room_server.exe (a multiplayer room server) with clang-cl, ENet and zstd: from vcpkg (x64-windows,
rem `vcpkg install enet zstd`; zstd.dll is copied next to the exe), else built from deps/enet and deps/zstd.
rem Usage: build.bat <output dir>   (default: this folder's build\)
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%build"
rem before vcenv: Visual Studio's environment points VCPKG_ROOT at its own vcpkg
set "VCPKG_TREE="
for %%v in ("%VCPKG_ROOT%" "%VCPKG_INSTALLATION_ROOT%") do (
    if not defined VCPKG_TREE if exist "%%~v\installed\x64-windows\lib\enet.lib" if exist "%%~v\installed\x64-windows\lib\zstd.lib" set "VCPKG_TREE=%%~v\installed\x64-windows"
)
call "%HERE%..\vcenv.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if defined VCPKG_TREE (
    echo room: ENet and zstd from %VCPKG_TREE%
    "%NX_CC%" /nologo /O2 /MD /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /I"%VCPKG_TREE%\include" "%HERE%room_server.c" ^
      /Fo"%OUT%\\" /Fe"%OUT%\room_server.exe" /link "%VCPKG_TREE%\lib\enet.lib" "%VCPKG_TREE%\lib\zstd.lib" ws2_32.lib winmm.lib || exit /b 1
    copy /y "%VCPKG_TREE%\bin\zstd.dll" "%OUT%\" >nul
    goto built
)
set "ENET=%HERE%..\..\..\deps\enet"
if not exist "%ENET%\host.c" (echo room: no ENet: vcpkg install enet zstd, or put it in %ENET% & exit /b 1)
set "ZSTD=%HERE%..\..\..\deps\zstd\lib"
if not exist "%ZSTD%\zstd.h" (echo room: no zstd: vcpkg install enet zstd, or put it in %ZSTD% & exit /b 1)
"%NX_CC%" /nologo /O2 /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /DZSTD_DISABLE_ASM /I"%ENET%\include" /I"%ZSTD%" ^
  "%HERE%room_server.c" "%ENET%\callbacks.c" "%ENET%\compress.c" "%ENET%\host.c" "%ENET%\list.c" ^
  "%ENET%\packet.c" "%ENET%\peer.c" "%ENET%\protocol.c" "%ENET%\win32.c" ^
  "%ZSTD%\common\*.c" "%ZSTD%\decompress\*.c" ^
  /Fo"%OUT%\\" /Fe"%OUT%\room_server.exe" /link ws2_32.lib winmm.lib || exit /b 1
:built
echo built %OUT%\room_server.exe
