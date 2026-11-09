@echo off
rem Build this game through the root build.bat (root CMakeLists.txt, preset "violet"). Usage: build.bat [ninja targets / options]
call "%~dp0..\..\build.bat" violet %*
