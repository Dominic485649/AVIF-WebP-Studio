vcpkg_download_distfile(ARCHIVE
    URLS "https://storage.googleapis.com/aom-releases/libaom-3.14.1.tar.gz"
    FILENAME "libaom-3.14.1.tar.gz"
    SHA512 a4c3427db0bb4cd49b8873ffae0a287570b86d6f0733da52798b08470519755ffe08198222be86d3bc8c9bd0052477017dc595b98bbbd77f76bfe7b9ce48d7fa
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        aom-rename-static.diff
        aom-uninitialized-pointer.diff
)

vcpkg_find_acquire_program(PERL)

if(VCPKG_TARGET_ARCHITECTURE MATCHES "^(x86|x64)$")
    vcpkg_find_acquire_program(NASM)
    set(aom_nasm_compiler "-DCMAKE_ASM_NASM_COMPILER=${NASM}")
endif()

set(aom_target_cpu "")
if(VCPKG_TARGET_IS_UWP OR (VCPKG_TARGET_IS_WINDOWS AND VCPKG_TARGET_ARCHITECTURE MATCHES "^arm"))
    set(aom_target_cpu "-DAOM_TARGET_CPU=generic")
endif()

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm" AND VCPKG_TARGET_IS_LINUX)
    set(aom_target_cpu "-DENABLE_NEON=OFF")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${aom_target_cpu}
        -DENABLE_DOCS=OFF
        -DENABLE_EXAMPLES=OFF
        -DENABLE_TESTDATA=OFF
        -DENABLE_TESTS=OFF
        -DENABLE_TOOLS=OFF
        -DTHREADS_PREFER_PTHREAD_FLAG=ON
        ${aom_nasm_compiler}
        "-DPERL_EXECUTABLE=${PERL}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/AOM)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
