# nxrecomp: the shared part of a game's build. A game's CMakeLists.txt includes this file and calls
#   nx_game(GAME_JSON <path to game.json> [SOURCES <game-specific .c files>])
# which builds <name>.exe (game.json "name") from the shared runtime, the game's sources and the lifted code in
# LIFTED_DIR (written by tools/lifter/a64lift.py). Configure from a vcvars64 shell with clang-cl and Ninja.
set(NX_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(NX_DEPS "${NX_ROOT}/../deps")
# third-party locations, found in this order (each can be set on the cmake command line instead):
#   Vulkan headers: deps/Vulkan-Headers, then the Vulkan SDK ($VULKAN_SDK)
#   SDL3: deps/SDL3/SDL3-<version> (the VC development zip), then vcpkg ($VCPKG_ROOT, C:/vcpkg, D:/vcpkg)
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
    foreach(_v "$ENV{VCPKG_ROOT}" "C:/vcpkg" "D:/vcpkg")
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
# untyped); -O1 keeps compile time reasonable for the very large generated functions
set(NX_LIFT_FLAGS /clang:-O1 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-march=haswell
                  /clang:-fbracket-depth=1024 /clang:-Wno-everything
                  /clang:-gline-tables-only /clang:-gcodeview)   # lines = guest offsets (a64lift #line)

set(NX_RUNTIME_SOURCES
    main.c loader.c dispatch.c mem.c kernel.c kmem.c svc.c ipc.c
    services/am.c services/lm.c services/apm.c services/pctl.c services/vi.c services/hid.c services/time.c
    services/acc.c services/mii.c services/audio.c audio_dsp.c audio_fx.c services/pl.c services/set.c
    services/nifm.c services/friends.c services/ldn.c services/bcat.c services/system.c services/audout.c services/hwopus.c services/offline.c services/fs.c romfs.c
    nvn.c gpu.c tex.c vk.c astc.c input.c movie.c)
list(TRANSFORM NX_RUNTIME_SOURCES PREPEND "${NX_ROOT}/runtime/")

function(nx_game)
    cmake_parse_arguments(G "" "GAME_JSON" "SOURCES" ${ARGN})
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
]] @ONLY)

    set(LIFTED_DIR "${build}/lifted" CACHE PATH "C sources written by tools/lifter/a64lift.py")
    file(GLOB lifted_sources CONFIGURE_DEPENDS "${LIFTED_DIR}/*.c")
    add_library(lifted STATIC ${lifted_sources})
    target_include_directories(lifted PRIVATE "${NX_ROOT}/runtime" "${LIFTED_DIR}")
    target_compile_options(lifted PRIVATE ${NX_LIFT_FLAGS})
    # each compile of a ~2 MB generated file takes 1-2 GB: limit parallel jobs so a 16 GB machine does not page
    set(LIFT_JOBS 8 CACHE STRING "parallel compiles of lifted code")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS lift_pool=${LIFT_JOBS})
    set_property(TARGET lifted PROPERTY JOB_POOL_COMPILE lift_pool)

    add_library(a64rt STATIC "${NX_ROOT}/runtime/a64rt.c")
    target_include_directories(a64rt PUBLIC "${NX_ROOT}/runtime")
    target_compile_options(a64rt PRIVATE ${NX_LIFT_FLAGS})

    # the game: the runtime (kernel, SVCs, services) around the lifted code
    add_executable(${NX_NAME} ${NX_RUNTIME_SOURCES} ${G_SOURCES})
    target_compile_options(${NX_NAME} PRIVATE /clang:-O2 /clang:-march=haswell /W3 /clang:-Wno-unused-parameter
                                              /clang:-Wno-bitwise-op-parentheses /clang:-fno-strict-aliasing)
    target_compile_definitions(${NX_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    target_include_directories(${NX_NAME} PRIVATE "${NX_ROOT}/runtime" "${CMAKE_BINARY_DIR}"
                               "${NX_VULKAN_INCLUDE}" "${NX_SDL3_INCLUDE}")
    target_link_libraries(${NX_NAME} PRIVATE lifted a64rt dbghelp bcrypt psapi mfplat mfreadwrite mfuuid ole32
                          "${NX_SDL3_LIB}")
    add_custom_command(TARGET ${NX_NAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       "${NX_SDL3_DLL}" "$<TARGET_FILE_DIR:${NX_NAME}>")
    # /DEBUG: a PDB whose public symbols name the lifted functions (f_<guest address>) in crash reports
    # /HIGHENTROPYVA:NO: otherwise Windows puts thread stacks and heaps anywhere up to 1 TB, inside the guest range.
    # /MAP: shows where lifted code lies (kernel.c preempts threads only inside it; see dispatch_init)
    target_link_options(${NX_NAME} PRIVATE /DEBUG /INCREMENTAL:NO /HIGHENTROPYVA:NO /MAP)
endfunction()
