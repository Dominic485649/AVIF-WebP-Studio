vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO haasn/libplacebo
    REF "v${VERSION}"
    SHA512 209b1713cff34f06149af16fb3ea52e3662a566ef5df6b29811ad295aa8cb6388f827a93fc8e0eed1a72f35b3b3aae835520c933079e706a51d11136a8128799
    HEAD_REF master
)

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    # MSVC's current C mode exposes the header but not C11 atomics.  This CPU
    # subset has no atomic users, so prevent the upstream common header from
    # including it; Windows thread helpers remain the upstream implementation.
    vcpkg_replace_string("${SOURCE_PATH}/src/common.h"
        "#if !defined(__cplusplus) || defined(__cpp_lib_stdatomic_h)\n#define PL_HAVE_STDATOMIC\n#endif\n\n#ifdef PL_HAVE_STDATOMIC"
        "#if (!defined(__cplusplus) || defined(__cpp_lib_stdatomic_h)) && !(defined(_MSC_VER) && !defined(__clang__))\n#define PL_HAVE_STDATOMIC\n#endif\n\n#ifdef PL_HAVE_STDATOMIC")
    vcpkg_replace_string("${SOURCE_PATH}/src/pl_thread_win32.h"
        "PL_THREAD_VOID (*fun)(void *),"
        "unsigned (__stdcall *fun)(void *),")
    vcpkg_replace_string("${SOURCE_PATH}/src/common.h"
        "#define pl_unreachable() (assert(!\"unreachable\"), __builtin_unreachable())"
        "#define pl_unreachable() (assert(!\"unreachable\"), __assume(0))")
endif()

# AWJ only needs libplacebo's documented CPU colorspace, tone-mapping and
# gamut-mapping APIs. Build that upstream source subset directly: Meson's
# all-or-nothing renderer target otherwise pulls shader generators and GPU
# dependencies even when every renderer backend is disabled.
vcpkg_cmake_configure(
    SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}"
    OPTIONS
        "-DLIBPLACEBO_SOURCE_DIR=${SOURCE_PATH}"
)
vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
