# MagelightCP — Magelight on Cyberpunk 2077: manifest mods drawn over the game (README.md).
# Its own project: the Skyrim host's build is untouched.
cmake_minimum_required(VERSION 3.24)
project(MagelightCP VERSION 0.1.0 LANGUAGES CXX)
# The one source of the version: the log, RED4ext's plugin info and the page's window.__MAGELIGHT__ read these.
add_compile_definitions(MLCP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MLCP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MLCP_VERSION_PATCH=${PROJECT_VERSION_PATCH} MLCP_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ULTRALIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/ultralight")
if(NOT EXISTS "${ULTRALIGHT_DIR}/include/Ultralight/Ultralight.h")
    message(FATAL_ERROR "Ultralight SDK not found at ${ULTRALIGHT_DIR} (extern/README.md).")
endif()

# The game patch's RED4ext SDK. No default: it is a checkout on this machine (README.md).
set(RED4EXT_SDK_DIR "" CACHE PATH "RED4ext.SDK checkout for game 2.5")
if(NOT EXISTS "${RED4EXT_SDK_DIR}/include/RED4ext/RED4ext.hpp")
    message(FATAL_ERROR "Set RED4EXT_SDK_DIR to a RED4ext.SDK checkout for game 2.5.")
endif()

# nlohmann/json, one header, fetched by build.ps1 into extern/ (gitignored).
set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/nlohmann/include")
if(NOT EXISTS "${JSON_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR "nlohmann/json.hpp not found under ${JSON_DIR} (build.ps1 fetches it).")
endif()

# The host without the game: everything but the hooks and the D3D12 drawing.
set(HOST_SOURCES
    src/Manifest.cpp
    src/Views.cpp
    src/Platform.cpp
    src/Bridge.cpp
    src/Input.cpp
    src/Api.cpp
    src/Log.cpp
    ${ULTRALIGHT_DIR}/platform/src/fonts/FontLoaderWin.cpp
    ${ULTRALIGHT_DIR}/platform/src/fonts/TextAnalysisSource.cpp)
set_source_files_properties(
    ${ULTRALIGHT_DIR}/platform/src/fonts/FontLoaderWin.cpp
    ${ULTRALIGHT_DIR}/platform/src/fonts/TextAnalysisSource.cpp
    PROPERTIES COMPILE_OPTIONS "/wd5043")

add_library(MagelightCP SHARED src/Plugin.cpp src/Compositor.cpp src/Session.cpp src/Natives.cpp ${HOST_SOURCES})

target_compile_definitions(MagelightCP PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    RED4EXT_HEADER_ONLY)
target_compile_options(MagelightCP PRIVATE /W3 /utf-8 /EHsc /MP)
target_include_directories(MagelightCP SYSTEM PRIVATE
    ${ULTRALIGHT_DIR}/include
    ${ULTRALIGHT_DIR}/platform/include
    ${JSON_DIR}
    ${RED4EXT_SDK_DIR}/include
    ${RED4EXT_SDK_DIR}/vendor/D3D12MemAlloc)
target_link_directories(MagelightCP PRIVATE ${ULTRALIGHT_DIR}/lib)
target_link_libraries(MagelightCP PRIVATE Ultralight UltralightCore WebCore dwrite d3d12 dxgi d3dcompiler delayimp Normaliz)
# Delay-loaded: Plugin.cpp loads them from the plugin's own folder before the first call.
target_link_options(MagelightCP PRIVATE
    /DELAYLOAD:Ultralight.dll /DELAYLOAD:UltralightCore.dll /DELAYLOAD:WebCore.dll)

# The host without the game (tools/view_test.cpp): manifests, pages, bridge and input, driven by raw key and mouse.
add_executable(mlcp_view_test tools/view_test.cpp ${HOST_SOURCES})
target_compile_definitions(mlcp_view_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(mlcp_view_test PRIVATE /W3 /utf-8 /EHsc)
target_include_directories(mlcp_view_test SYSTEM PRIVATE
    ${ULTRALIGHT_DIR}/include ${ULTRALIGHT_DIR}/platform/include ${JSON_DIR})
target_link_directories(mlcp_view_test PRIVATE ${ULTRALIGHT_DIR}/lib)
target_link_libraries(mlcp_view_test PRIVATE Ultralight UltralightCore WebCore dwrite user32 delayimp Normaliz)
target_link_options(mlcp_view_test PRIVATE
    /DELAYLOAD:Ultralight.dll /DELAYLOAD:UltralightCore.dll /DELAYLOAD:WebCore.dll)
# It loads the stage's renamed DLLs, so it gets their names too.
add_custom_command(TARGET mlcp_view_test POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/rename_ultralight.ps1"
        $<TARGET_FILE:mlcp_view_test>)

# The stage: the game's folders, as an MO2 mod holds them.
set(STAGE_ROOT "${CMAKE_BINARY_DIR}/stage")
set(STAGE "${STAGE_ROOT}/red4ext/plugins/MagelightCP")
add_custom_command(TARGET MagelightCP POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MagelightCP> "${STAGE}"
    # Ultralight's DLLs under Magelight's names, and every reference to them (tools/rename_ultralight.ps1).
    COMMAND ${CMAKE_COMMAND} -E copy "${ULTRALIGHT_DIR}/bin/Ultralight.dll" "${STAGE}/Magelight1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${ULTRALIGHT_DIR}/bin/UltralightCore.dll" "${STAGE}/Magelight1Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${ULTRALIGHT_DIR}/bin/WebCore.dll" "${STAGE}/MgWCore.dll"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${STAGE}/Ultralight.dll" "${STAGE}/UltralightCore.dll" "${STAGE}/WebCore.dll"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/rename_ultralight.ps1"
        "${STAGE}/MagelightCP.dll" "${STAGE}/Magelight1.dll" "${STAGE}/Magelight1Core.dll" "${STAGE}/MgWCore.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ULTRALIGHT_DIR}/resources" "${STAGE}/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/scripts/reds"
        "${STAGE_ROOT}/r6/scripts/Magelight"
    # The example mod: its pages, and its redscript half.
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/examples/MagelightDemo"
        "${STAGE_ROOT}/r6/magelight/MagelightDemo"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_ROOT}/r6/scripts/MagelightDemo"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/examples/MagelightDemo.reds"
        "${STAGE_ROOT}/r6/scripts/MagelightDemo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cet/magelight"
        "${STAGE_ROOT}/bin/x64/plugins/cyber_engine_tweaks/mods/magelight")
