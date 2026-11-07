# nxrecomp: the shared part of a game's build. A game's CMakeLists.txt includes this file and calls
#   nx_game(GAME_JSON <path to game.json> [SOURCES <game-specific .c files>])
# which builds <name>.exe (game.json "name") from the shared runtime, the game's sources and the lifted code in
# LIFTED_DIR (written by tools/lifter/a64lift.py), and nx_mod(<name> SOURCES ...) for mods (DLLs, runtime/nxmod.h).
# Configure from a vcvars64 shell with clang-cl and Ninja.
set(NX_ROOT "${CMAKE_CURRENT_LIST_DIR}")
enable_language(CXX)                        # the menu (ui/nxui.cpp) and mods are C++
set(NX_DEPS "${NX_ROOT}/../deps")
# third-party locations, found in this order (each can be set on the cmake command line instead):
#   Vulkan headers: deps/Vulkan-Headers, then the Vulkan SDK ($VULKAN_SDK)
#   SDL3: deps/SDL3/SDL3-<version> (the VC development zip), then vcpkg ($VCPKG_ROOT, or Visual Studio's own
#   vcpkg, $VCPKG_INSTALLATION_ROOT)
if(NOT NX_VULKAN_INCLUDE)
    if(EXISTS "${NX_DEPS}/Vulkan-Headers/include/vulkan/vulkan.h")
        set(_vk "${NX_DEPS}/Vulkan-Headers/include")
    elseif(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}/Include" _vk)
    else()
        message(FATAL_ERROR "No Vulkan headers: put Vulkan-Headers in deps/, install the Vulkan SDK, or set NX_VULKAN_INCLUDE")
    endif()
    set(NX_VULKAN_INCLUDE "${_vk}" CACHE PATH "Vulkan headers")
endif()
if(NOT NX_SDL3_INCLUDE)
    file(GLOB _sdl_dirs "${NX_DEPS}/SDL3/SDL3-*")
    set(_roots)
    foreach(_d ${_sdl_dirs})
        list(APPEND _roots "${_d}|include|lib/x64/SDL3.lib|lib/x64/SDL3.dll")
    endforeach()
    foreach(_v "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
        if(_v)
            file(TO_CMAKE_PATH "${_v}" _v)
            list(APPEND _roots "${_v}/installed/x64-windows|include|lib/SDL3.lib|bin/SDL3.dll")
        endif()
    endforeach()
    foreach(_r ${_roots})
        string(REPLACE "|" ";" _p "${_r}")
        list(GET _p 0 _base)
        list(GET _p 1 _inc)
        list(GET _p 2 _lib)
        list(GET _p 3 _dll)
        if(EXISTS "${_base}/${_inc}/SDL3/SDL.h" AND EXISTS "${_base}/${_lib}")
            set(NX_SDL3_INCLUDE "${_base}/${_inc}" CACHE PATH "SDL3 headers")
            set(NX_SDL3_LIB "${_base}/${_lib}" CACHE FILEPATH "SDL3 import library")
            set(NX_SDL3_DLL "${_base}/${_dll}" CACHE FILEPATH "SDL3 DLL, copied next to the exe")
            break()
        endif()
    endforeach()
    if(NOT NX_SDL3_INCLUDE)
        message(FATAL_ERROR "No SDL3: put the SDL3 VC development zip in deps/SDL3, install it with vcpkg, or set NX_SDL3_INCLUDE, NX_SDL3_LIB and NX_SDL3_DLL")
    endif()
endif()
message(STATUS "nxrecomp: Vulkan headers ${NX_VULKAN_INCLUDE}, SDL3 ${NX_SDL3_INCLUDE}")

# no CMake default flags for lifted code: Debug's /Zi /RTC1 multiply compile time and object size
set(CMAKE_C_FLAGS_RELEASE "/MD" CACHE STRING "" FORCE)

# lifted code: no FP contraction (the guest fuses only where it says so), no strict aliasing (guest memory is
# untyped); -O1 keeps compile time reasonable for the very large generated functions (NX_LIFT_OPT to compare)
set(NX_LIFT_OPT "-O1" CACHE STRING "clang optimization level for lifted code")
set(NX_LIFT_FLAGS /clang:${NX_LIFT_OPT} /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-march=haswell
                  /clang:-fbracket-depth=1024 /clang:-Wno-everything
                  /clang:-gline-tables-only /clang:-gcodeview)   # lines = guest offsets (a64lift #line)
# loads and stores with SP as the base (a64rt.h LDS / STS) are volatile like the rest; ON makes them plain, to compare
option(NX_STACK_PLAIN "plain (non-volatile) stack accesses in lifted code" OFF)
if(NX_STACK_PLAIN)
    list(APPEND NX_LIFT_FLAGS /DNX_STACK_PLAIN)
endif()

set(NX_RUNTIME_SOURCES
    main.c loader.c dispatch.c mem.c kernel.c kmem.c svc.c ipc.c
    services/am.c services/lm.c services/apm.c services/pctl.c services/vi.c services/hid.c services/time.c
    services/acc.c services/mii.c services/audio.c audio_dsp.c audio_fx.c services/pl.c services/set.c
    services/nifm.c services/friends.c services/ldn.c services/bcat.c services/system.c services/audout.c services/hwopus.c services/offline.c services/fs.c romfs.c
    nca.c image.c nvn.c gpu.c tex.c vk.c astc.c input.c options.c mods.c ui.c movie.c swkbd.c)
list(TRANSFORM NX_RUNTIME_SOURCES PREPEND "${NX_ROOT}/runtime/")

# NETPLAY (nx_game option): local wireless over community rooms (runtime/net, services/ldn_net.c, services/bsd.c),
# with ENet and zstd, found in this order:
#   vcpkg (x64-windows: `vcpkg install enet zstd`): the vcpkg SDL3 came from, then $VCPKG_ROOT and
#   $VCPKG_INSTALLATION_ROOT; DLLs go next to the exe
#   deps/enet (v1.3.18) and deps/zstd (v1.5.6, lib/ only), built from source
set(NX_NETPLAY_SOURCES net/room.c net/lobby.c net/browser.c net/netmenu.c services/ldn_net.c services/bsd.c)
list(TRANSFORM NX_NETPLAY_SOURCES PREPEND "${NX_ROOT}/runtime/")

# defines nx_enet and nx_zstd; NX_NETPLAY_DLLS lists DLLs to copy next to the exe
function(nx_netplay_libs)
    if(TARGET nx_enet)
        return()
    endif()
    set(NX_NETPLAY_DLLS "" CACHE INTERNAL "")
    # the vcpkg tree SDL3 came from first: Visual Studio's environment points VCPKG_ROOT at its own vcpkg
    get_filename_component(_sdl_base "${NX_SDL3_INCLUDE}" DIRECTORY)
    set(_bases "${_sdl_base}")
    foreach(_v "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
        if(_v)
            file(TO_CMAKE_PATH "${_v}/installed/x64-windows" _b)
            list(APPEND _bases "${_b}")
        endif()
    endforeach()
    foreach(_b ${_bases})
        if(EXISTS "${_b}/include/enet/enet.h" AND EXISTS "${_b}/lib/enet.lib" AND EXISTS "${_b}/include/zstd.h"
           AND EXISTS "${_b}/lib/zstd.lib")
            message(STATUS "nxrecomp: netplay libraries from vcpkg (${_b})")
            add_library(nx_enet INTERFACE)
            target_include_directories(nx_enet INTERFACE "${_b}/include")
            target_link_libraries(nx_enet INTERFACE "${_b}/lib/enet.lib" ws2_32 winmm)
            add_library(nx_zstd INTERFACE)
            target_include_directories(nx_zstd INTERFACE "${_b}/include")
            target_link_libraries(nx_zstd INTERFACE "${_b}/lib/zstd.lib")
            file(GLOB _dlls "${_b}/bin/enet.dll" "${_b}/bin/zstd.dll")
            set(NX_NETPLAY_DLLS "${_dlls}" CACHE INTERNAL "")
            return()
        endif()
    endforeach()
    foreach(_d enet zstd)
        if(NOT EXISTS "${NX_DEPS}/${_d}")
            message(FATAL_ERROR "NETPLAY needs ENet and zstd: `vcpkg install enet zstd` (x64-windows), or deps/${_d} (see nxrecomp.cmake)")
        endif()
    endforeach()
    message(STATUS "nxrecomp: netplay libraries built from deps/enet and deps/zstd")
    set(_e "${NX_DEPS}/enet")
    add_library(nx_enet STATIC ${_e}/callbacks.c ${_e}/compress.c ${_e}/host.c ${_e}/list.c ${_e}/packet.c
                               ${_e}/peer.c ${_e}/protocol.c ${_e}/win32.c)
    target_include_directories(nx_enet BEFORE PUBLIC "${_e}/include")
    target_compile_definitions(nx_enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    target_compile_options(nx_enet PRIVATE /clang:-O2 /clang:-Wno-everything)
    target_link_libraries(nx_enet PUBLIC ws2_32 winmm)
    set(_z "${NX_DEPS}/zstd/lib")
    file(GLOB _zsrc "${_z}/common/*.c" "${_z}/compress/*.c" "${_z}/decompress/*.c")
    add_library(nx_zstd STATIC ${_zsrc})
    # ahead of the SDL3 / Vulkan include folders: a vcpkg one may have its own zstd.h, which imports zstd from a DLL
    target_include_directories(nx_zstd BEFORE PUBLIC "${_z}")
    target_compile_definitions(nx_zstd PRIVATE ZSTD_DISABLE_ASM)
    target_compile_options(nx_zstd PRIVATE /clang:-O2 /clang:-Wno-everything)
endfunction()

# libopus for hwopus (services/hwopus.c): vcpkg (x64-windows: `vcpkg install opus`), in the vcpkg tree SDL3 came
# from, then $VCPKG_ROOT and $VCPKG_INSTALLATION_ROOT. Defines nx_opus; NX_OPUS_DLL is copied next to the exe.
function(nx_opus_lib)
    if(TARGET nx_opus)
        return()
    endif()
    get_filename_component(_sdl_base "${NX_SDL3_INCLUDE}" DIRECTORY)
    set(_bases "${_sdl_base}")
    foreach(_v "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
        if(_v)
            file(TO_CMAKE_PATH "${_v}/installed/x64-windows" _b)
            list(APPEND _bases "${_b}")
        endif()
    endforeach()
    foreach(_b ${_bases})
        if(EXISTS "${_b}/include/opus/opus.h" AND EXISTS "${_b}/lib/opus.lib")
            message(STATUS "nxrecomp: libopus from vcpkg (${_b})")
            add_library(nx_opus INTERFACE)
            target_include_directories(nx_opus INTERFACE "${_b}/include")
            target_link_libraries(nx_opus INTERFACE "${_b}/lib/opus.lib")
            set(NX_OPUS_DLL "${_b}/bin/opus.dll" CACHE INTERNAL "")
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "No libopus (hwopus): `vcpkg install opus` (x64-windows)")
endfunction()

function(nx_game)
    cmake_parse_arguments(G "NETPLAY" "GAME_JSON" "SOURCES" ${ARGN})
    get_filename_component(G_GAME_JSON "${G_GAME_JSON}" ABSOLUTE)
    get_filename_component(game_dir "${G_GAME_JSON}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${G_GAME_JSON}")
    file(READ "${G_GAME_JSON}" json)
    string(JSON NX_NAME GET "${json}" name)
    string(JSON NX_TITLE GET "${json}" title)
    string(JSON title_id GET "${json}" title_id)
    string(JSON dump GET "${json}" dump)
    string(JSON build GET "${json}" build)
    # owned add-on content (DLC) indices, e.g. "dlc": [1]
    set(NX_DLC "")
    string(JSON ndlc ERROR_VARIABLE no_dlc LENGTH "${json}" dlc)
    if(NOT no_dlc AND ndlc GREATER 0)
        math(EXPR last "${ndlc} - 1")
        foreach(i RANGE ${last})
            string(JSON d GET "${json}" dlc ${i})
            list(APPEND NX_DLC "${d}")
        endforeach()
    endif()
    # default frame rate, e.g. "framerate": 60 (only for games whose hooks step the game by the frame time;
    # absent: the game's own, FRAMERATE_GAME); NX_FRAMERATE / NX_NOPACE and F5 still change it
    string(JSON NX_DEFAULT_FRAMERATE ERROR_VARIABLE no_fr GET "${json}" framerate)
    if(no_fr)
        set(NX_DEFAULT_FRAMERATE -1)
    endif()
    # the menu's theme, e.g. "theme": {"background": "#17121f", "panel": "#241b33", "accent": "#8b5cf6",
    # "accent2": "#f5c542", "text": "#f3eefc", "rounding": 6} (nxui derives ImGui's colours; absent: ImGui's dark)
    set(NX_UI_THEME "")
    string(JSON _theme ERROR_VARIABLE no_theme GET "${json}" theme)
    if(NOT no_theme)
        string(JSON _n LENGTH "${_theme}")
        if(_n GREATER 0)
            math(EXPR _last "${_n} - 1")
            foreach(_i RANGE ${_last})
                string(JSON _k MEMBER "${_theme}" ${_i})
                string(JSON _v GET "${_theme}" "${_k}")
                string(APPEND NX_UI_THEME "${_k}=${_v};")
            endforeach()
        endif()
    endif()
    list(LENGTH NX_DLC NX_DLC_COUNT)
    list(APPEND NX_DLC 0)                               # a C array may not be empty
    list(JOIN NX_DLC ", " NX_DLC)
    get_filename_component(dump "${dump}" ABSOLUTE BASE_DIR "${game_dir}")
    get_filename_component(build "${build}" ABSOLUTE BASE_DIR "${game_dir}")
    # the executable finds the dump relative to itself, as the image and save folders
    file(RELATIVE_PATH NX_GAME_DIR "${CMAKE_BINARY_DIR}" "${dump}")
    set(NX_TITLE_ID "0x${title_id}ull")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/game_config.h" CONTENT [[
/* generated by nxrecomp.cmake from @G_GAME_JSON@ */
#pragma once
#define NX_NAME "@NX_NAME@"
#define NX_TITLE "@NX_TITLE@"
#define NX_TITLE_ID @NX_TITLE_ID@
#define NX_GAME_DIR "@NX_GAME_DIR@"
#define NX_ROOT "@NX_ROOT@"
#define NX_DLC_COUNT @NX_DLC_COUNT@
#define NX_DLC_INDICES {@NX_DLC@}
#define NX_DEFAULT_FRAMERATE (@NX_DEFAULT_FRAMERATE@)
#define NX_UI_THEME "@NX_UI_THEME@"
]] @ONLY)

    set(LIFTED_DIR "${build}/lifted" CACHE PATH "C sources written by tools/lifter/a64lift.py")
    file(GLOB lifted_sources CONFIGURE_DEPENDS "${LIFTED_DIR}/*.c")
    add_library(lifted STATIC ${lifted_sources})
    target_include_directories(lifted PRIVATE "${NX_ROOT}/runtime" "${LIFTED_DIR}")
    target_compile_options(lifted PRIVATE ${NX_LIFT_FLAGS})
    # each compile of a ~2 MB generated file takes 1-2 GB: limit parallel jobs so a 16 GB machine does not page
    set(LIFT_JOBS 0 CACHE STRING "parallel compiles of lifted code (0: by cores and memory)")
    set(lift_jobs ${LIFT_JOBS})
    if(lift_jobs LESS 1)                # a lifted file takes up to ~1.3 GB to compile: 2 GB each
        cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
        cmake_host_system_information(RESULT mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
        math(EXPR lift_jobs "${mem_mb} / 2048")
        if(lift_jobs GREATER cores)
            set(lift_jobs ${cores})
        endif()
        if(lift_jobs LESS 1)
            set(lift_jobs 1)
        endif()
    endif()
    message(STATUS "nxrecomp: ${lift_jobs} parallel compiles of lifted code (LIFT_JOBS)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS lift_pool=${lift_jobs})
    set_property(TARGET lifted PROPERTY JOB_POOL_COMPILE lift_pool)

    add_library(a64rt STATIC "${NX_ROOT}/runtime/a64rt.c")
    target_include_directories(a64rt PUBLIC "${NX_ROOT}/runtime")
    target_compile_options(a64rt PRIVATE ${NX_LIFT_FLAGS})

    # the package's copies at -O3 (best speed for players; a second full compile of the lifted code, made only when
    # the package is built)
    string(REPLACE "/clang:${NX_LIFT_OPT}" "/clang:-O3" lift_o3 "${NX_LIFT_FLAGS}")
    add_library(lifted_o3 STATIC EXCLUDE_FROM_ALL ${lifted_sources})
    target_include_directories(lifted_o3 PRIVATE "${NX_ROOT}/runtime" "${LIFTED_DIR}")
    target_compile_options(lifted_o3 PRIVATE ${lift_o3})
    set_property(TARGET lifted_o3 PROPERTY JOB_POOL_COMPILE lift_pool)
    add_library(a64rt_o3 STATIC EXCLUDE_FROM_ALL "${NX_ROOT}/runtime/a64rt.c")
    target_include_directories(a64rt_o3 PUBLIC "${NX_ROOT}/runtime")
    target_compile_options(a64rt_o3 PRIVATE ${lift_o3})

    # zstd: the shader pack (gpu.c) in every build, netplay's compression; ENet with it
    nx_netplay_libs()
    nx_opus_lib()
    # the shader packer (tools/shader/pack.c): translate.py --spv's output in one file for players
    add_executable(nxshpack "${NX_ROOT}/tools/shader/pack.c")
    target_link_libraries(nxshpack PRIVATE nx_zstd)
    target_compile_options(nxshpack PRIVATE /clang:-O2)
    foreach(_dll ${NX_NETPLAY_DLLS})
        add_custom_command(TARGET nxshpack POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           "${_dll}" "$<TARGET_FILE_DIR:nxshpack>")
    endforeach()

    # the menu (nxui.dll), then the game: the runtime (kernel, SVCs, services) around the lifted code
    nx_ui_dll()
    nx_game_exe(${NX_NAME} OFF)
    # the same for players (NX_PACKAGE: the game read from their own NSPs in game\, data under %LOCALAPPDATA%, no
    # console, no developer keys), named after the game; built on request (`--target <name>_pkg`, package.ps1)
    nx_game_exe(${NX_NAME}_pkg ON)
    set_target_properties(${NX_NAME}_pkg PROPERTIES OUTPUT_NAME "${NX_TITLE}" RUNTIME_OUTPUT_DIRECTORY
                          "${CMAKE_BINARY_DIR}/package" EXCLUDE_FROM_ALL ON)
endfunction()

# one executable of the game: the runtime around the lifted code (shared libraries lifted and a64rt)
function(nx_game_exe target package)
    add_executable(${target} ${NX_RUNTIME_SOURCES} ${G_SOURCES})
    if(package)
        set(opt /clang:-O3)
        set(libs lifted_o3 a64rt_o3)
    else()
        set(opt /clang:-O2)
        set(libs lifted a64rt)
    endif()
    target_compile_options(${target} PRIVATE ${opt} /clang:-march=haswell /W3 /clang:-Wno-unused-parameter
                                              /clang:-Wno-bitwise-op-parentheses /clang:-fno-strict-aliasing
                                              /clang:-gline-tables-only /clang:-gcodeview)   # static functions named in profiles
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    target_include_directories(${target} PRIVATE "${NX_ROOT}/runtime" "${CMAKE_BINARY_DIR}"
                               "${NX_VULKAN_INCLUDE}" "${NX_SDL3_INCLUDE}")
    target_link_libraries(${target} PRIVATE ${libs} nx_zstd nx_opus dbghelp bcrypt psapi mfplat mfreadwrite mfuuid ole32 cfgmgr32 gdi32
                          windowscodecs "${NX_SDL3_LIB}")
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       "${NX_SDL3_DLL}" "$<TARGET_FILE_DIR:${target}>")
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       "${NX_OPUS_DLL}" "$<TARGET_FILE_DIR:${target}>")
    # /DEBUG: a PDB whose public symbols name the lifted functions (f_<guest address>) in crash reports
    # /HIGHENTROPYVA:NO: otherwise Windows puts thread stacks and heaps anywhere up to 1 TB, inside the guest range.
    # /MAP: shows where lifted code lies (kernel.c preempts threads only inside it; see dispatch_init)
    target_link_options(${target} PRIVATE /DEBUG /INCREMENTAL:NO /HIGHENTROPYVA:NO /MAP)
    add_dependencies(${target} nxui)            # loaded at run time from next to the exe (ui.c)
    if(package)
        target_compile_definitions(${target} PRIVATE NX_PACKAGE)
        target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
    foreach(_dll ${NX_NETPLAY_DLLS})            # zstd's (and ENet's) DLLs when they come from vcpkg
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           "${_dll}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
    if(G_NETPLAY)
        target_sources(${target} PRIVATE ${NX_NETPLAY_SOURCES})
        target_compile_definitions(${target} PRIVATE NX_NETPLAY)
        target_link_libraries(${target} PRIVATE nx_enet winhttp comctl32)
    endif()
endfunction()

# Dear ImGui (the menu, nxui.dll, and mods) from vcpkg (x64-windows: `vcpkg install imgui[vulkan-binding]`), in the
# vcpkg tree SDL3 came from, then $VCPKG_ROOT and $VCPKG_INSTALLATION_ROOT. Sets NX_IMGUI_BASE.
function(nx_imgui_find)
    if(NOT NX_IMGUI_BASE)
        get_filename_component(_sdl_base "${NX_SDL3_INCLUDE}" DIRECTORY)
        set(_bases "${_sdl_base}")
        foreach(_v "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
            if(_v)
                file(TO_CMAKE_PATH "${_v}/installed/x64-windows" _b)
                list(APPEND _bases "${_b}")
            endif()
        endforeach()
        foreach(_b ${_bases})
            if(EXISTS "${_b}/include/imgui_impl_vulkan.h" AND EXISTS "${_b}/lib/imgui.lib")
                set(NX_IMGUI_BASE "${_b}" CACHE PATH "vcpkg tree with Dear ImGui (imgui[vulkan-binding])")
                break()
            endif()
        endforeach()
        if(NOT NX_IMGUI_BASE)
            message(FATAL_ERROR "the menu and mods need Dear ImGui: `vcpkg install imgui[vulkan-binding]` (x64-windows)")
        endif()
        message(STATUS "nxrecomp: Dear ImGui from ${NX_IMGUI_BASE}")
    endif()
endfunction()

# The menu over the game (ui/nxui.cpp, runtime/ui_host.h): a DLL next to the exe (ui.c loads it), with Dear ImGui and
# its Vulkan backend. A DLL because the runtime's Vulkan function pointers have Vulkan's own names (vk.h).
function(nx_ui_dll)
    if(TARGET nxui)
        return()
    endif()
    nx_imgui_find()
    add_library(nxui SHARED "${NX_ROOT}/ui/nxui.cpp")
    target_include_directories(nxui PRIVATE "${NX_VULKAN_INCLUDE}" "${NX_ROOT}/runtime" "${NX_SDL3_INCLUDE}"
                               "${NX_IMGUI_BASE}/include")
    target_compile_definitions(nxui PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(nxui PRIVATE /clang:-O2 /W3)
    target_link_libraries(nxui PRIVATE "${NX_IMGUI_BASE}/lib/imgui.lib" "${NX_IMGUI_BASE}/lib/vulkan-1.lib"
                          "${NX_SDL3_LIB}")
    set_target_properties(nxui PROPERTIES CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    # the package's copy (package.ps1 takes the folder as it is)
    add_custom_command(TARGET nxui POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/package"
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nxui>" "${CMAKE_BINARY_DIR}/package")
endfunction()

# A mod (runtime/nxmod.h): a DLL built into <build>/mods, which the runtime loads from mods\ next to the exe; it draws
# into the menu's Dear ImGui through modkit/. Each mod is optional: one DLL each.
#   nx_mod(<name> SOURCES <files>)
function(nx_mod name)
    cmake_parse_arguments(M "" "" "SOURCES" ${ARGN})
    nx_imgui_find()
    add_library(${name} SHARED ${M_SOURCES} "${NX_ROOT}/modkit/modkit.cpp")
    # the Vulkan headers ahead of vcpkg's include folder, as the runtime has them
    target_include_directories(${name} PRIVATE "${NX_VULKAN_INCLUDE}" "${NX_ROOT}/runtime" "${NX_ROOT}/modkit"
                               "${NX_SDL3_INCLUDE}" "${NX_IMGUI_BASE}/include")
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${name} PRIVATE /clang:-O2 /W3)
    # ImGui's Vulkan backend was built with prototypes: vulkan-1.lib (the loader the runtime uses too)
    target_link_libraries(${name} PRIVATE "${NX_IMGUI_BASE}/lib/imgui.lib" "${NX_SDL3_LIB}")
    set_target_properties(${name} PROPERTIES CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods"
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods")
endfunction()
