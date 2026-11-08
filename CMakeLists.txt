# violetrecomp: Pokemon Violet 4.0.0 recompiled for Windows x64, on the shared runtime and tools in ../../nxrecomp.
# Configure from a vcvars64 shell with clang-cl and Ninja (see build.bat).
cmake_minimum_required(VERSION 3.24)
# no CMake default flags for lifted code: Debug's /Zi /RTC1 multiply compile time and object size.
# (must come before project(), which otherwise picks Debug for clang-cl)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (default) or Debug")
project(violetrecomp C CXX)          # C++: the mods (ImGui)
include("${CMAKE_SOURCE_DIR}/../../nxrecomp/nxrecomp.cmake")
nx_game(GAME_JSON "${CMAKE_SOURCE_DIR}/game.json" SOURCES "${CMAKE_SOURCE_DIR}/fps.c" "${CMAKE_SOURCE_DIR}/gfx.c" "${CMAKE_SOURCE_DIR}/lua.c" "${CMAKE_SOURCE_DIR}/wild.c" "${CMAKE_SOURCE_DIR}/raidgem.c" NETPLAY)
# mods (nxrecomp/runtime/nxmod.h), each optional, loaded from <build>/cmake/mods; their windows are in the menu's Mods
# list. Data in <build>/mods: raids.dat (mods/raids/raiddata.py), items.txt and mons_*.txt (tools/svedit)
nx_mod(violet_raids SOURCES "${CMAKE_SOURCE_DIR}/mods/raids/raids.cpp")           # Tera raids (F6)
nx_mod(violet_items SOURCES "${CMAKE_SOURCE_DIR}/mods/items/items.cpp")           # Items (F7)
nx_mod(violet_mons SOURCES "${CMAKE_SOURCE_DIR}/mods/mons/mons.cpp")              # Spawn a Pokemon (F8)
foreach(_m violet_raids violet_items violet_mons)
    target_include_directories(${_m} PRIVATE "${CMAKE_SOURCE_DIR}/mods/common")
endforeach()
