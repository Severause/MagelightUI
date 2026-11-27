# Overlay port for RED4ext.SDK, header-only, for the Cyberpunk 2077 host (cyberpunk/). Pinned to the commit that
# updated its types for game patch 2.5: the SDK's class layouts are compiled in, so one SDK build serves one patch.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO WopsS/RED4ext.SDK
    REF 5246e659d783e5834cc08a90e56c8932c9ed3449
    SHA512 3b1aea24b53dbad259b0326267d953d742800f1e995c6e1120ef07287af4d4fe4b718b9e925c47fb1987e095405cdc55827315835c74ff1a145df31a0ecd0e5f
    HEAD_REF master
)

set(VCPKG_BUILD_TYPE release)   # headers only

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRED4EXT_HEADER_ONLY=ON
        -DRED4EXT_INSTALL=ON
        -DRED4EXT_BUILD_EXAMPLES=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME RED4ext CONFIG_PATH lib/cmake/RED4ext)
# Its pkg-config file carries the build's absolute paths; consumers use the CMake config.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/RED4ext/pkgconfig")

# The SDK's GpuApi headers include <D3D12MemAlloc.h>, which its install leaves out.
file(INSTALL "${SOURCE_PATH}/vendor/D3D12MemAlloc/D3D12MemAlloc.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md" "${SOURCE_PATH}/THIRD_PARTY_LICENSES.md")
