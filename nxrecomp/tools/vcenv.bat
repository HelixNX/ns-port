@echo off
rem Sets up the MSVC x64 environment and NX_CC (clang-cl) for building a game. Called by each game's build.bat.
rem Overrides: NX_VCVARS (a vcvars64.bat), NX_CC (a clang-cl.exe). Otherwise Visual Studio is found with vswhere
rem (installed with every Visual Studio) and clang-cl is Visual Studio's LLVM component, else MSYS2's.
if not defined NX_VCVARS (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "NX_VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined NX_VCVARS (echo vcenv: no Visual Studio with the C++ x64 tools found; set NX_VCVARS & exit /b 1)
if not exist "%NX_VCVARS%" (echo vcenv: %NX_VCVARS% does not exist & exit /b 1)
rem Visual Studio's own scripts run vswhere.exe from PATH (else "'vswhere.exe' is not recognized")
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%NX_VCVARS%" >nul || exit /b 1
if not defined NX_CC if exist "%VCINSTALLDIR%Tools\Llvm\x64\bin\clang-cl.exe" set "NX_CC=%VCINSTALLDIR%Tools\Llvm\x64\bin\clang-cl.exe"
if not defined NX_CC if exist "C:\msys64\mingw64\bin\clang-cl.exe" set "NX_CC=C:\msys64\mingw64\bin\clang-cl.exe"
if not defined NX_CC (echo vcenv: no clang-cl found: install Visual Studio's "C++ Clang tools" or set NX_CC & exit /b 1)
set "NX_CC=%NX_CC:\=/%"
exit /b 0
