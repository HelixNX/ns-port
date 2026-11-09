# ns-port: the one CMake entry. A build tree holds one game, picked with NX_GAME; CMakePresets.json has a preset per
# game (its build tree is the game's own <project>/build/cmake). From the root:
#   build.bat violet [ninja targets / options]      (sets up clang-cl through nxrecomp/tools/vcenv.bat)
#   cmake --preset violet && cmake --build --preset violet     (in a shell where vcenv.bat has run)
# Adding a game: its folder holds game.json and game.cmake (calls nx_game); add it to the list below and a preset.
cmake_minimum_required(VERSION 3.24)
# no CMake default flags for lifted code: Debug's /Zi /RTC1 multiply compile time and object size.
# (must come before project(), which otherwise picks Debug for clang-cl)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (default) or Debug")

set(NX_GAME_violet "pkmn-violet/violetrecomp")
set(NX_GAME_mk8 "mk8dx/mk8recomp")
set(NX_GAMES violet mk8)
set(NX_GAME "" CACHE STRING "the game this build tree builds: ${NX_GAMES}")
set_property(CACHE NX_GAME PROPERTY STRINGS ${NX_GAMES})
if(NOT NX_GAME IN_LIST NX_GAMES)
    message(FATAL_ERROR "set NX_GAME to one of: ${NX_GAMES} (or use a preset: cmake --preset <game>)")
endif()
set(NX_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${NX_GAME_${NX_GAME}}")

# the project is named after the game (game.json "name"), as each game's own CMakeLists.txt did before
file(READ "${NX_GAME_DIR}/game.json" _json)
string(JSON _name GET "${_json}" name)
project(${_name} C)

include("${CMAKE_CURRENT_SOURCE_DIR}/nxrecomp/nxrecomp.cmake")
include("${NX_GAME_DIR}/game.cmake")
