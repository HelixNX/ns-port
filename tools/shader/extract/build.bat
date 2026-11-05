@echo off
rem Builds oozbatch.exe (ooz's Oodle decoder, for extract.py) with clang-cl from deps\ooz
rem (git clone https://github.com/powzix/ooz deps\ooz). Usage: build.bat <output dir>  (extract.py: <build>\ooz)
setlocal
set "HERE=%~dp0"
set "OOZ=%HERE%..\..\..\..\deps\ooz"
set "OUT=%~1"
if "%OUT%"=="" (echo usage: build.bat ^<output dir^> & exit /b 1)
if not exist "%OOZ%\kraken.cpp" (echo no %OOZ%\kraken.cpp: clone github.com/powzix/ooz into deps\ooz & exit /b 1)
call "%HERE%..\..\vcenv.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
rem ooz is written for MSVC, which includes sys\stat.h on its own
"%NX_CC%" /nologo /O2 /EHsc /MD /w /FIsys/types.h /FIsys/stat.h /I"%OOZ%" "%HERE%oozbatch.cpp" "%OOZ%\bitknit.cpp" ^
    "%OOZ%\lzna.cpp" /Fo"%OUT%\\" /Fe"%OUT%\oozbatch.exe" || exit /b 1
del "%OUT%\*.obj" 2>nul
