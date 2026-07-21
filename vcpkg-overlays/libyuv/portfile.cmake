vcpkg_download_distfile(ARCHIVE
    URLS "https://codeload.github.com/lemenkov/libyuv/tar.gz/afff0cf68bdd2d713c81040a4bd1b7c7fe69e39c"
    FILENAME "libyuv-afff0cf68bdd2d713c81040a4bd1b7c7fe69e39c.tar.gz"
    SHA512 658002f6018534b29881a05ee44877afa13811760fa2be34cdbebe7a35bff672f9f7a5489bfab979d95208b66fa39fa6b0972c2edea1ce731e9e981bf970f844
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        cmake.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools BUILD_TOOLS
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
    OPTIONS_DEBUG
        -DBUILD_TOOLS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES yuvconvert yuvconstants AUTO_CLEAN)
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libyuv/basic_types.h" "defined(LIBYUV_USING_SHARED_LIBRARY)" "1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(COPY "${CMAKE_CURRENT_LIST_DIR}/libyuv-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")
if(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    file(APPEND "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" [[

Attention:
You are using MSVC to compile libyuv. This build won't compile any of the acceleration codes.
]])
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
