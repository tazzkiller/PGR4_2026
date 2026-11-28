# pgr4 - ReXGlue recompiled Project Gotham Racing 4 (Title Update #2) with pgr4-native's
# D3D12 renderer (vendored in pgr4-native/, MIT; see pgr4-native/VERSION).
#
# SDK boilerplate lives in generated/rexglue.cmake. The SDK must be the patched tree in
# ../rexglue-sdk (commit bd833a2 + pgr4-native's patches), installed to out/install/win-amd64.

cmake_minimum_required(VERSION 3.25)
project(pgr4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "The native renderer is D3D12: build for Windows.")
endif()

# pgr4-native's renderer sources, the SDK source tree it borrows translator and texture-format
# sources from, and the FidelityFX SDK headers (upscaler / frame generation, DLLs loaded at run time).
set(PGR4_NATIVE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/pgr4-native/src")
set(PGR4_NATIVE_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/pgr4-native/common")
set(PGR4_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-sdk")
set(PGR4_FIDELITYFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/fidelityfx-sdk")
# NVIDIA Streamline (DLSS; local addition, native/streamline.h): headers, and the signed production
# DLLs staged beside the executable (sl.interposer.dll is loaded at run time, nothing links it).
set(PGR4_STREAMLINE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/streamline")

include(generated/rexglue.cmake)

file(GLOB PGR4_NATIVE_SOURCES CONFIGURE_DEPENDS "${PGR4_NATIVE_SRC}/native/*.cpp"
    # MFG Unlocker's RTX 40 patches (local addition, native/mfg_unlock.h).
    "${PGR4_NATIVE_SRC}/native/mfg_unlock/*.cpp")
add_executable(pgr4 WIN32
    # The game's icon (local addition).
    res/pgr4.rc
    src/main.cpp
    src/pgr4_hooks.cpp
    src/pgr4_menu.cpp
    src/pgr4_profile.cpp
    src/pgr4_autopilot.cpp
    src/pgr4_freecam.cpp
    src/pgr4_audio_probe.cpp
    "${PGR4_NATIVE_COMMON}/title_launch.cpp"
    "${PGR4_NATIVE_SRC}/pgr4_d3d_trace.cpp"
    ${PGR4_NATIVE_SOURCES}
    # Texture format tables the renderer shares with the Xenos plugin, which exports none of them.
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp"
    # The settings overlay's renderer (native/overlay.h): Dear ImGui's DX12 backend, beside the
    # ImGui core the SDK links in (same version, its source tree).
    "${PGR4_REXGLUE_SDK_SRC}/thirdparty/imgui/backends/imgui_impl_dx12.cpp"
)
set_source_files_properties("${PGR4_REXGLUE_SDK_SRC}/thirdparty/imgui/backends/imgui_impl_dx12.cpp"
    PROPERTIES COMPILE_OPTIONS "-w" SKIP_PRECOMPILE_HEADERS ON)
target_include_directories(pgr4 PRIVATE "${PGR4_NATIVE_SRC}" "${PGR4_NATIVE_COMMON}")
target_include_directories(pgr4 SYSTEM PRIVATE
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include"
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/framegeneration/include"
    "${PGR4_STREAMLINE_SDK}/include"
    "${PGR4_REXGLUE_SDK_SRC}/thirdparty/imgui/backends")

# The Xenos plugin still runs fences and interrupts (and is the fallback, F9).
rexglue_setup_target(pgr4 GPU_PLUGINS xenos)
target_link_libraries(pgr4 PRIVATE d3d12 dxgi dxguid d3dcompiler cabinet)  # cabinet: the HD pack's Decompressor
# The overlay's "Save" writes pgr4.toml beside the executable and here, so the POST_BUILD copy
# below does not undo it (native/overlay.cpp).
target_compile_definitions(pgr4 PRIVATE PGR4_SOURCE_TOML="${CMAKE_CURRENT_SOURCE_DIR}/pgr4.toml"
    PGR4_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}")
# Intel's XeGTAO (MIT, local addition 16/11/2026, native/scene_effects.cpp): the menu's second
# ambient occlusion. Its header for the constants' layout, and its shaders embedded (#embed) and
# compiled at run time as the other scene effects.
set(PGR4_XEGTAO "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/XeGTAO")
target_compile_definitions(pgr4 PRIVATE PGR4_XEGTAO_H="${PGR4_XEGTAO}/XeGTAO.h"
    PGR4_XEGTAO_HLSLI="${PGR4_XEGTAO}/XeGTAO.hlsli")
# Its MIT license with the game (licenses\, which tools\export_release.ps1 exports).
add_custom_command(TARGET pgr4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pgr4>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PGR4_XEGTAO}/LICENSE"
        "$<TARGET_FILE_DIR:pgr4>/licenses/XeGTAO-LICENSE.txt")
include("${PGR4_NATIVE_SRC}/native/shader_xlate.cmake")
target_link_libraries(pgr4 PRIVATE pgr4_shader_xlate)

# Write pgr4.pdb (the recompiled code already carries -gline-tables-only) so profilers and cdb
# can name the sub_XXXXXXXX functions.
target_link_options(pgr4 PRIVATE -g)
# Local change (18/11/2026): line tables for the renderer's and the hooks' sources too, as the
# recompiled code has, so a profile (xperf, llvm-symbolizer) gives their lines. Debug information
# only: the optimizations are the same.
set_source_files_properties(${PGR4_NATIVE_SOURCES} "${PGR4_NATIVE_SRC}/pgr4_d3d_trace.cpp"
    src/pgr4_hooks.cpp PROPERTIES COMPILE_OPTIONS "-gline-tables-only")

# Replacement textures (textures\<hash>.bin, pgr4-native textures.cpp: the title screen's logo
# with its 2026 tag, made by tools/logo_2026/make_logo.py), beside the executable.
add_custom_command(TARGET pgr4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/textures" "$<TARGET_FILE_DIR:pgr4>/textures")
# HD textures shipped loose, outside the HD pack (textures_hd\<hash>.dds over the pack's:
# pgr4-native textures.cpp Hd), so that one fixed texture does not mean a new pack to download
# (18/11/2026: the photo mode's focus ring, 4F4F8FBFC959A3DE).
add_custom_command(TARGET pgr4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/textures_hd" "$<TARGET_FILE_DIR:pgr4>/textures_hd")

# Replacement pixel shaders (shaders\<ucode hash>.ps.hlsl, pgr4-native shader_replace.h).
add_custom_command(TARGET pgr4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:pgr4>/shaders")

# A fresh clone has no pgr4.toml (each install has its own, not versioned): made from the defaults.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pgr4.toml")
    file(COPY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/pgr4.default.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/pgr4.toml")
endif()

# Runtime settings and AMD's signed FidelityFX DLLs, beside the executable.
add_custom_command(TARGET pgr4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/pgr4.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/pipelines_base.bin"
        "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
        "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
        "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.interposer.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.common.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.dlss.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/nvngx_dlss.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.pcl.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.dlss_g.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/nvngx_dlssg.dll"
        "${PGR4_STREAMLINE_SDK}/bin/x64/sl.reflex.dll"
        "$<TARGET_FILE_DIR:pgr4>"
    VERBATIM)

# generate_exception_handlers emits SEH_TRY/SEH_CATCH_ALL/SEH_END, but the generated
# pgr4_pch.h does not include the header that defines them, and on Windows SEH_CATCH uses
# GetExceptionCode()/GetExceptionInformation(), which are <excpt.h> macros.
if(TARGET pgr4_recomp)
    target_precompile_headers(pgr4_recomp PRIVATE
        "$<$<PLATFORM_ID:Windows>:<excpt.h$<ANGLE-R>>"
        <rex/platform/exceptions.h>)
endif()

# Local change (26/11/2026, branch cpu_thread): the recompiled code's guest loads and stores are
# relaxed atomics (the SDK's pch_h.inja, REX_GUEST_LOAD...). ON gives back the volatile accesses of
# before (clang's -fms-volatile: each an acquire or a release), the same machine code as before, for
# A/B builds. Compile-time only: it cannot be switched while the game runs. ON by default since
# 27/11/2026 (Tazz: no risk taken, the volatile accesses back in the release); OFF for the relaxed ones.
option(PGR4_VOLATILE_GUEST_ACCESS "Recompiled code: volatile guest memory accesses, as before" ON)
if(TARGET pgr4_recomp AND PGR4_VOLATILE_GUEST_ACCESS)
    target_compile_definitions(pgr4_recomp PRIVATE REX_VOLATILE_GUEST_ACCESS)
endif()
if(TARGET pgr4_recomp)
    # Built with the recompiled code's pch and definitions: run.log says which accesses it uses.
    target_sources(pgr4_recomp PRIVATE src/pgr4_guest_access.cpp)
endif()

# The release's launcher (src/launcher/launcher.cpp): Direct2D, no SDK; the C++ runtime linked in,
# so it runs, and says so, where the Visual C++ Redistributable the game needs is missing.
add_executable(pgr4_launcher WIN32 src/launcher/launcher.cpp res/pgr4.rc)
set_target_properties(pgr4_launcher PROPERTIES OUTPUT_NAME "PGR4Launcher"
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
# pgr4.default.toml built in (#embed): written beside the executables when pgr4.toml is missing.
target_compile_definitions(pgr4_launcher PRIVATE UNICODE _UNICODE
    PGR4_SOURCE_TOML="${CMAKE_CURRENT_SOURCE_DIR}/pgr4.toml"
    PGR4_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PGR4_DEFAULT_TOML="${CMAKE_CURRENT_SOURCE_DIR}/pgr4.default.toml")
target_compile_options(pgr4_launcher PRIVATE -Wno-c23-extensions)
target_link_libraries(pgr4_launcher PRIVATE d2d1 d3d12 dwrite dxgi ole32 shell32 xinput iphlpapi
    ws2_32 dwmapi uuid windowscodecs)
# SDL3 for the controllers (local change: PlayStation ones too, and their family for the button
# hints): its own static build from the SDK's sources, with the launcher's static C runtime (the
# SDK's links the DLL one) and without what the launcher does not use.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(sdl_off SDL_AUDIO SDL_RENDER SDL_GPU SDL_CAMERA SDL_DIALOG SDL_TRAY SDL_POWER SDL_SENSOR)
    set(${sdl_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(pgr4_runtime_saved "${CMAKE_MSVC_RUNTIME_LIBRARY}")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_subdirectory("${PGR4_REXGLUE_SDK_SRC}/thirdparty/sdl3" "${CMAKE_CURRENT_BINARY_DIR}/sdl3_launcher"
    EXCLUDE_FROM_ALL)
set(CMAKE_MSVC_RUNTIME_LIBRARY "${pgr4_runtime_saved}")
# The target itself: SDL3::SDL3-static is the SDK install's, imported with it.
target_link_libraries(pgr4_launcher PRIVATE SDL3-static)
# Title Update 2 built into the launcher, installed by it beside itself when the game has none.
# Off by default: the files are Microsoft's, for a build of one's own copy, not for publishing.
option(PGR4_LAUNCHER_EMBED_TITLE_UPDATE "Build Title Update 2 into PGR4Launcher.exe" OFF)
set(PGR4_TITLE_UPDATE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../title-update/extracted" CACHE PATH
    "The extracted Title Update 2 (default.xexp, gw.xexp, Pgr4TitleUpdate.bin)")
if(PGR4_LAUNCHER_EMBED_TITLE_UPDATE)
    foreach(file default.xexp gw.xexp Pgr4TitleUpdate.bin)
        if(NOT EXISTS "${PGR4_TITLE_UPDATE_DIR}/${file}")
            message(FATAL_ERROR "PGR4_LAUNCHER_EMBED_TITLE_UPDATE: no ${PGR4_TITLE_UPDATE_DIR}/${file}")
        endif()
    endforeach()
    target_compile_definitions(pgr4_launcher PRIVATE
        PGR4_TU_DEFAULT_XEXP="${PGR4_TITLE_UPDATE_DIR}/default.xexp"
        PGR4_TU_GW_XEXP="${PGR4_TITLE_UPDATE_DIR}/gw.xexp"
        PGR4_TU_BIN="${PGR4_TITLE_UPDATE_DIR}/Pgr4TitleUpdate.bin")
    target_compile_options(pgr4_launcher PRIVATE -Wno-c23-extensions)
    set_property(SOURCE src/launcher/launcher.cpp APPEND PROPERTY OBJECT_DEPENDS
        "${PGR4_TITLE_UPDATE_DIR}/default.xexp" "${PGR4_TITLE_UPDATE_DIR}/gw.xexp"
        "${PGR4_TITLE_UPDATE_DIR}/Pgr4TitleUpdate.bin")
endif()
# The DLC packages of PGR4_DLC_DIR built into the launcher, written by it beside itself and installed
# by the game at its start (dlc_root). Off by default, as the title update: for one's own DLC.
option(PGR4_LAUNCHER_EMBED_DLC "Build the DLC packages of PGR4_DLC_DIR into PGR4Launcher.exe" OFF)
set(PGR4_DLC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dlc" CACHE PATH
    "PGR4's DLC packages (STFS files, as copied off a console)")
if(PGR4_LAUNCHER_EMBED_DLC)
    file(GLOB PGR4_DLC_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${PGR4_DLC_DIR}/*")
    if(NOT PGR4_DLC_FILES)
        message(FATAL_ERROR "PGR4_LAUNCHER_EMBED_DLC: no package in ${PGR4_DLC_DIR}")
    endif()
    set(dlc_data "")
    set(dlc_list "")
    set(dlc_index 0)
    foreach(file ${PGR4_DLC_FILES})
        get_filename_component(name "${file}" NAME)
        string(APPEND dlc_data "const unsigned char kDlc${dlc_index}[] = {
#embed \"${file}\"
};
")
        string(APPEND dlc_list "    {\"${name}\", kDlc${dlc_index}, sizeof(kDlc${dlc_index})},
")
        math(EXPR dlc_index "${dlc_index} + 1")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/launcher_dlc.inc"
        "${dlc_data}const EmbeddedFile kEmbeddedDlc[] = {
${dlc_list}};
")
    target_compile_definitions(pgr4_launcher PRIVATE
        PGR4_DLC_INC="${CMAKE_CURRENT_BINARY_DIR}/launcher_dlc.inc")
    target_compile_options(pgr4_launcher PRIVATE -Wno-c23-extensions)
    set_property(SOURCE src/launcher/launcher.cpp APPEND PROPERTY OBJECT_DEPENDS
        ${PGR4_DLC_FILES} "${CMAKE_CURRENT_BINARY_DIR}/launcher_dlc.inc")
endif()
