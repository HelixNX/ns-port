# violetrecomp: Pokemon Violet 4.0.0 recompiled for Windows x64, on the shared runtime and tools in nxrecomp/.
# Included by the root CMakeLists.txt with NX_GAME=violet: build from the root (build.bat violet, or the CMake preset
# "violet" in a vcenv shell), or with this folder's build.bat.
nx_game(GAME_JSON "${CMAKE_CURRENT_LIST_DIR}/game.json" SOURCES "${CMAKE_CURRENT_LIST_DIR}/fps.c" "${CMAKE_CURRENT_LIST_DIR}/gfx.c" "${CMAKE_CURRENT_LIST_DIR}/lua.c" "${CMAKE_CURRENT_LIST_DIR}/wild.c" "${CMAKE_CURRENT_LIST_DIR}/raidgem.c" NETPLAY)
# mods (nxrecomp/runtime/nxmod.h), each optional, loaded from <build>/cmake/mods; their windows are in the menu's Mods
# list. Data in <build>/mods: raids.dat (mods/raids/raiddata.py), items.txt and mons_*.txt (tools/svedit)
nx_mod(violet_raids SOURCES "${CMAKE_CURRENT_LIST_DIR}/mods/raids/raids.cpp")           # Tera raids (F6)
nx_mod(violet_items SOURCES "${CMAKE_CURRENT_LIST_DIR}/mods/items/items.cpp")           # Items (F7)
nx_mod(violet_mons SOURCES "${CMAKE_CURRENT_LIST_DIR}/mods/mons/mons.cpp")              # Spawn a Pokemon (F8)
foreach(_m violet_raids violet_items violet_mons)
    target_include_directories(${_m} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/mods/common")
endforeach()
