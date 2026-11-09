@echo off
rem Configure (first time) and build with clang-cl + Ninja. Usage: build.bat [ninja targets / options]
rem Tools are found by nxrecomp\tools\vcenv.bat; third-party libraries by nxrecomp.cmake.
set ROOT=%~dp0..
call "%ROOT%\..\nxrecomp\tools\vcenv.bat" || exit /b 1
if not exist "%ROOT%\build\cmake\build.ninja" (
    cmake -G Ninja -S "%~dp0." -B "%ROOT%\build\cmake" -DCMAKE_C_COMPILER="%NX_CC%" -DCMAKE_CXX_COMPILER="%NX_CC%" ^
        -DLIFTED_DIR="%ROOT%\build\lifted" || exit /b 1
)
cmake --build "%ROOT%\build\cmake" %*
