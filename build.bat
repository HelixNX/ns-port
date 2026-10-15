@echo off
rem Configure (first time) and build with clang-cl + Ninja. Usage: build.bat [ninja targets / options]
call "E:\Microsoft Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0..
if not exist "%ROOT%\build\cmake\build.ninja" (
    cmake -G Ninja -S "%~dp0." -B "%ROOT%\build\cmake" ^
        -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/clang-cl.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe ^
        -DLIFTED_DIR="%ROOT%\build\lifted" || exit /b 1
)
cmake --build "%ROOT%\build\cmake" %*
