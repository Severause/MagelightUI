# Overlay port for CommonLibSSE-NG, pinned to MinLL/CommonLibVR v4.39.5.
#
# Why this fork: Skyrim 1.7.99 (2026-08-20) and 1.7.104 (2026-08-27) ship a
# FORMAT-5 address library. alandtse/CommonLibSSE-NG 4.17.0 classifies 1.7.x
# as SE (REL::Module::ClassifyRuntime keys on the MINOR version), so it asks
# for version-1-7-104-0.bin at format 1, does not find it, and terminates in
# stl::report_and_fail. Upstream relicensed MIT -> GPL-3.0 on 2026-07-25
# (last MIT tag v4.39.3) BEFORE adding format-5 support, and its modding
# exception does not extend to plugin code, so we cannot take that fix.
#
# MinLL/CommonLibVR (branch ng) is the MIT continuation, branched from
# v4.39.3. Verified against the tarball at this REF: MIT LICENSE; minor >= 6
# classifies as AE so 1.7.x hits every AE gate; kFormatDense = 5 with its own
# shared-mapping name (CommonLibVR-Offsets-f5-) so it cannot collide with
# another library's table; the real versionlib-1-7-99-0.bin and
# -1-7-104-0.bin are committed under tests/REL; and
# kVersionIndependentEx_AddressLibraryV5 is defined AND set automatically by
# UsesAddressLibrary(), which clears the SKSE 2.3.0+ gate with no source
# change on our side.
#
# Do NOT bump extern/openvr alongside this. 4.39.5's submodule moves to SDK
# 1.0.15, which renames the interface version STRINGS (IVRSystem_017 -> _019,
# IVRCompositor_021 -> _022, IVROverlay_016 -> _018). Those strings are baked
# into runtime VR_GetGenericInterface lookups; a mismatch makes the VR
# presenter go dormant silently. openvr stays at ebdea152 (SDK 1.0.10).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MinLL/CommonLibVR
    REF 550cc4fb9114649dcf526d1f3d73d710c5d7003b
    SHA512 a053d852b682d597acc9e7684740ab95f212e79542265a3e379ff0132bb333592051ddaa2ffc81b1f85fdbda080afe360f5dc7a54d15d1aa7982a98865ce7243
    HEAD_REF ng
)

# --- VR support (SeverActions v3.0.1) -------------------------------------
# GitHub tarballs omit submodules, so extern/openvr arrives empty and VR could
# not build (this is why VR was disabled). Fetch the exact openvr commit that
# CommonLibVR f7ff3ec5 pins, into the layout its CMake hardcodes
# (extern/openvr/{headers,lib/win64/openvr_api.lib}).
vcpkg_from_github(
    OUT_SOURCE_PATH OPENVR_SOURCE_PATH
    REPO ValveSoftware/openvr
    REF ebdea152f8aac77e9a6db29682b81d762159df7e
    SHA512 4fb668d933ac5b73eb4e97eb29816176e500a4eaebe2480cd0411c95edfb713d58312036f15db50884a2ef5f4ca44859e108dec2b982af9163cefcfc02531f63
    HEAD_REF master
)
file(GLOB _openvr_items "${OPENVR_SOURCE_PATH}/*")
file(COPY ${_openvr_items} DESTINATION "${SOURCE_PATH}/extern/openvr")

# VR crash fix (the same fix SkyrimNet carries): CommonLibVR v4.17.0 routes the
# non-const BSPointerHandle::GetSmartPointer to address-library id 12785, which
# has no VR mapping — VR users crash on the first handle deref (dialogue /
# subtitle / nearby-actor). Re-point VR to the const id 12204
# (LookupReferenceByHandle), which IS present and is what worked pre-v4.17.0.
# SE/AE ids are untouched. String-replace (not a .patch) to avoid whitespace
# fragility; fail loudly if the target line ever drifts on a future bump.
set(_bsph "${SOURCE_PATH}/include/RE/B/BSPointerHandle.h")
file(READ "${_bsph}" _bsph_txt)
string(FIND "${_bsph_txt}" "RELOCATION_ID(12785, 12922)" _bsph_found)
if(_bsph_found EQUAL -1)
    message(FATAL_ERROR "[SeverActions] VR BSPointerHandle fix: RELOCATION_ID(12785, 12922) not found — CommonLibVR changed; re-verify the VR id fix.")
endif()
string(REPLACE
    "RELOCATION_ID(12785, 12922)"
    "REL::RelocationID(12785, 12922, 12204)"
    _bsph_txt "${_bsph_txt}")
file(WRITE "${_bsph}" "${_bsph_txt}")
# --------------------------------------------------------------------------

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTS=OFF
        -DSKSE_SUPPORT_XBYAK=ON
        # SE + AE + VR universal build (openvr staged above; VR handle-id fix
        # applied above). The plugin delay-loads openvr_api.dll so the flat
        # SE/AE build has no hard dependency on it — see Native/CMakeLists.txt.
        -DENABLE_SKYRIM_VR=ON
)

vcpkg_cmake_install()

# Propagate the openvr headers to CONSUMERS. CommonLibVR's installed
# BSVRInterface.h does `#include "openvr.h"`, but CommonLib exposes the openvr
# include dir only as a BUILD_INTERFACE (used to build itself, not propagated to
# downstream). Install the openvr headers into the package include dir so plugins
# compiling against this VR-enabled CommonLib can resolve "openvr.h".
file(GLOB _openvr_headers "${SOURCE_PATH}/extern/openvr/headers/*.h")
file(INSTALL ${_openvr_headers} DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_cmake_config_fixup(PACKAGE_NAME CommonLibSSE CONFIG_PATH lib/cmake/CommonLibSSE)

# Ship the openvr import lib WITH the package and point the exported target at it.
# CommonLibVR links openvr_api.lib by ABSOLUTE build-tree path
# ($<$<BOOL:ON>:<SOURCE_PATH>/extern/openvr/lib/win64/openvr_api.lib>) and that path
# lands verbatim in the installed CommonLibSSE-targets.cmake. It resolves only on the
# machine that BUILT the port with its buildtrees intact; any consumer whose package
# came from vcpkg's binary cache (CI, a fresh clone, anyone after a buildtrees clean)
# fails to link: 'openvr_api.lib ... missing and no known rule to make it'
# (Magelight CI, first cached run, 2026-09-11).
file(INSTALL "${SOURCE_PATH}/extern/openvr/lib/win64/openvr_api.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(INSTALL "${SOURCE_PATH}/extern/openvr/lib/win64/openvr_api.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
file(GLOB_RECURSE _clvr_targets "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE-targets.cmake")
foreach(_t IN LISTS _clvr_targets)
    vcpkg_replace_string("${_t}"
        "${SOURCE_PATH}/extern/openvr/lib/win64/openvr_api.lib"
        "\${_IMPORT_PREFIX}/lib/openvr_api.lib")
endforeach()

# Some downstream projects look for CommonLibSSE.cmake at share/CommonLibSSE/.
# Mirror the upstream layout.
file(GLOB CMAKE_CONFIGS "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE/*.cmake")
if(CMAKE_CONFIGS)
    file(INSTALL ${CMAKE_CONFIGS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")
endif()
if(EXISTS "${SOURCE_PATH}/cmake/CommonLibSSE.cmake")
    file(INSTALL "${SOURCE_PATH}/cmake/CommonLibSSE.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")
endif()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE")

file(
    INSTALL "${SOURCE_PATH}/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME copyright
)
