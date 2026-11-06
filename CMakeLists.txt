# violetrecomp: Pokemon Violet 4.0.0 recompiled for Windows x64, on the shared runtime and tools in ../../nxrecomp.
# Configure from a vcvars64 shell with clang-cl and Ninja (see build.bat).
cmake_minimum_required(VERSION 3.24)
# no CMake default flags for lifted code: Debug's /Zi /RTC1 multiply compile time and object size.
# (must come before project(), which otherwise picks Debug for clang-cl)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (default) or Debug")
project(violetrecomp C CXX)          # C++: the mods (ImGui)
include("${CMAKE_SOURCE_DIR}/../../nxrecomp/nxrecomp.cmake")
nx_game(GAME_JSON "${CMAKE_SOURCE_DIR}/game.json" SOURCES "${CMAKE_SOURCE_DIR}/fps.c" "${CMAKE_SOURCE_DIR}/gfx.c" NETPLAY)
# mods (nxrecomp/runtime/nxmod.h), loaded from <build>/cmake/mods: the Tera raid editor (F6; its data: mods/raids/raiddata.py)
nx_mod(violet_raids SOURCES "${CMAKE_SOURCE_DIR}/mods/raids/raids.cpp" "${CMAKE_SOURCE_DIR}/mods/raids/items.cpp")
