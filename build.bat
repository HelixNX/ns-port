@echo off
rem Build one game from the root CMakeLists.txt: build.bat violet|mk8 [ninja targets / options]
rem Sets up clang-cl + MSVC through nxrecomp\tools\vcenv.bat, configures the game's preset (CMakePresets.json; its
rem build tree is <project>\build\cmake) the first time, then builds. Third-party libraries are found by nxrecomp.cmake.
setlocal
set GAME=%~1
if "%GAME%"=="" (echo usage: build.bat violet^|mk8 [ninja targets / options] & exit /b 1)
if /i "%GAME%"=="violet" (set BIN=%~dp0pkmn-violet\build\cmake) else if /i "%GAME%"=="mk8" (set BIN=%~dp0mk8dx\build\cmake) else (echo unknown game: %GAME% & exit /b 1)
rem the rest of the arguments go to cmake --build (%* keeps the game name, so collect them)
set ARGS=
:args
shift
if "%~1"=="" goto run
set ARGS=%ARGS% %1
goto args
:run
call "%~dp0nxrecomp\tools\vcenv.bat" || exit /b 1
rem presets are read from the current directory
pushd "%~dp0"
rem a build tree configured from the game's own folder (before the root CMakeLists.txt): configure it again from here
rem (build.ninja goes too, so a configure that fails is run again next time)
if exist "%BIN%\CMakeCache.txt" (findstr /b /c:"NX_GAME:" "%BIN%\CMakeCache.txt" >nul || del "%BIN%\CMakeCache.txt" "%BIN%\build.ninja" 2>nul)
set NX_RC=0
if not exist "%BIN%\build.ninja" (cmake --preset %GAME% || set NX_RC=1)
if "%NX_RC%"=="0" (cmake --build --preset %GAME% %ARGS% || set NX_RC=1)
popd
exit /b %NX_RC%
