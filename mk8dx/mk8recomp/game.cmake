# mk8recomp: MK8DX 3.0.5 recompiled for Windows x64, on the shared runtime and tools in nxrecomp/.
# Included by the root CMakeLists.txt with NX_GAME=mk8: build from the root (build.bat mk8, or the CMake preset "mk8"
# in a vcenv shell), or with this folder's build.bat.
# NETPLAY: local play over community rooms (the room browser at start-up)
nx_game(GAME_JSON "${CMAKE_CURRENT_LIST_DIR}/game.json" NETPLAY)
