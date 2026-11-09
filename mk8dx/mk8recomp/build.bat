@echo off
rem Build this game through the root build.bat (root CMakeLists.txt, preset "mk8"). Usage: build.bat [ninja targets / options]
call "%~dp0..\..\build.bat" mk8 %*
