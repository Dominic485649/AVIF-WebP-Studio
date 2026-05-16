cmake_minimum_required(VERSION 3.30)

project(AVIFWebPStudio
    VERSION 0.3.0
    DESCRIPTION "AVIF-WebP-Studio batch converter with MagickWand and Slint"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AVIF_STATIC_SLINT "Link Slint statically so AVIF-WebP-Studio does not need slint_cpp.dll" ON)
option(AVIF_STATIC_MSVC_RUNTIME "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" OFF)
option(AVIF_ENABLE_RELEASE_IPO "Enable interprocedural optimization for Release builds when the compiler supports it" OFF)

set(AVIF_RELEASE_IPO_SUPPORTED OFF)
if(AVIF_ENABLE_RELEASE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AVIF_RELEASE_IPO_SUPPORTED
        OUTPUT AVIF_RELEASE_IPO_ERROR
        LANGUAGES CXX)
    if(NOT AVIF_RELEASE_IPO_SUPPORTED)
        message(WARNING "Release IPO/LTO is not available: ${AVIF_RELEASE_IPO_ERROR}")
    endif()
endif()

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/${config}")
endforeach()

set(MAGICK_ROOT "" CACHE PATH "ImageMagick runtime root built by scripts/build-magick.ps1")
if(NOT MAGICK_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imagemagick-runtime/x64/Release/include/MagickWand/MagickWand.h")
        set(MAGICK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imagemagick-runtime/x64/Release"
            CACHE PATH "ImageMagick runtime root built by scripts/build-magick.ps1" FORCE)
    endif()
endif()

if(NOT EXISTS "${MAGICK_ROOT}/include/MagickWand/MagickWand.h")
    message(FATAL_ERROR "ImageMagick headers not found. Run scripts/build-magick.ps1 or configure with -DMAGICK_ROOT=<runtime root>.")
endif()

file(GLOB MAGICK_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${MAGICK_ROOT}/*.dll"
    "${MAGICK_ROOT}/bin/*.dll")
set(AVIF_MAGICK_STATIC_RUNTIME OFF)
if(NOT MAGICK_RUNTIME_DLLS)
    set(AVIF_MAGICK_STATIC_RUNTIME ON)
endif()

# The bundled ImageMagick runtime built by scripts/build-magick.ps1 is static.
# CMake Tools often reuses a dynamic /MD + x64-windows cache, which makes
# ImageMagick headers emit __declspec(dllimport) and later causes __imp_Magick*
# link failures. Keep the whole dependency graph on the static runtime.
if(MSVC AND AVIF_MAGICK_STATIC_RUNTIME AND NOT AVIF_STATIC_MSVC_RUNTIME)
    message(STATUS "Detected static ImageMagick runtime; forcing /MT and static vcpkg dependencies.")
    set(AVIF_STATIC_MSVC_RUNTIME ON CACHE BOOL
        "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" FORCE)
endif()

if(AVIF_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(VCPKG_ROOT "$ENV{VCPKG_ROOT}" CACHE PATH "vcpkg root directory")
if(NOT VCPKG_ROOT AND EXISTS "D:/Scoop/apps/vcpkg/current")
    set(VCPKG_ROOT "D:/Scoop/apps/vcpkg/current" CACHE PATH "vcpkg root directory" FORCE)
endif()
if(AVIF_STATIC_MSVC_RUNTIME)
    set(AVIF_DEFAULT_VCPKG_TRIPLET "x64-windows-static")
else()
    set(AVIF_DEFAULT_VCPKG_TRIPLET "x64-windows")
endif()
set(VCPKG_TRIPLET "${AVIF_DEFAULT_VCPKG_TRIPLET}" CACHE STRING "vcpkg target triplet")
if(AVIF_MAGICK_STATIC_RUNTIME AND NOT VCPKG_TRIPLET MATCHES "-static$")
    message(STATUS "Detected static ImageMagick runtime; forcing VCPKG_TRIPLET=x64-windows-static.")
    set(VCPKG_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet" FORCE)
endif()
if(VCPKG_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed/${VCPKG_TRIPLET}")
endif()

foreach(package_dir_var IN ITEMS scn_DIR FastFloat_DIR)
    if(DEFINED ${package_dir_var})
        string(FIND "${${package_dir_var}}" "/${VCPKG_TRIPLET}/" package_triplet_position)
        if(package_triplet_position EQUAL -1)
            message(STATUS "Clearing stale cached ${package_dir_var}: ${${package_dir_var}}")
            unset(${package_dir_var} CACHE)
        endif()
    endif()
endforeach()

message(STATUS "AVIF-WebP-Studio vcpkg triplet: ${VCPKG_TRIPLET}")
message(STATUS "AVIF-WebP-Studio MSVC runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")

find_package(scn CONFIG REQUIRED)

file(TO_CMAKE_PATH "${MAGICK_ROOT}" AVIF_MAGICK_ROOT_CMAKE)
get_filename_component(AVIF_MAGICK_ROOT_CONFIG "${MAGICK_ROOT}" NAME)
if(AVIF_MAGICK_ROOT_CONFIG STREQUAL "Debug")
    set(AVIF_MAGICK_LIBRARY_FLAVOR "DB")
else()
    set(AVIF_MAGICK_LIBRARY_FLAVOR "RL")
endif()
set(AVIF_MAGICK_LIBRARY_PREFIX "CORE_${AVIF_MAGICK_LIBRARY_FLAVOR}_")
message(STATUS "AVIF-WebP-Studio ImageMagick library flavor: ${AVIF_MAGICK_LIBRARY_PREFIX}")
foreach(magick_library_var IN ITEMS MAGICKWAND_LIBRARY MAGICKCORE_LIBRARY)
    if(DEFINED ${magick_library_var})
        file(TO_CMAKE_PATH "${${magick_library_var}}" magick_library_path)
        string(FIND "${magick_library_path}" "${AVIF_MAGICK_ROOT_CMAKE}/" magick_library_root_position)
        get_filename_component(magick_library_name "${${magick_library_var}}" NAME)
        string(FIND "${magick_library_name}" "${AVIF_MAGICK_LIBRARY_PREFIX}" magick_library_flavor_position)
        if(magick_library_root_position EQUAL -1 OR NOT magick_library_flavor_position EQUAL 0)
            message(STATUS "Clearing stale cached ${magick_library_var}: ${${magick_library_var}}")
            unset(${magick_library_var} CACHE)
        endif()
    endif()
endforeach()

find_library(MAGICKWAND_LIBRARY
    NAMES ${AVIF_MAGICK_LIBRARY_PREFIX}MagickWand_
    PATHS "${MAGICK_ROOT}/lib" "${MAGICK_ROOT}"
    NO_DEFAULT_PATH
    REQUIRED)
find_library(MAGICKCORE_LIBRARY
    NAMES ${AVIF_MAGICK_LIBRARY_PREFIX}MagickCore_
    PATHS "${MAGICK_ROOT}/lib" "${MAGICK_ROOT}"
    NO_DEFAULT_PATH
    REQUIRED)
file(GLOB MAGICK_ALL_LIBRARIES CONFIGURE_DEPENDS
    "${MAGICK_ROOT}/lib/${AVIF_MAGICK_LIBRARY_PREFIX}*.lib"
    "${MAGICK_ROOT}/${AVIF_MAGICK_LIBRARY_PREFIX}*.lib")
list(REMOVE_ITEM MAGICK_ALL_LIBRARIES "${MAGICKWAND_LIBRARY}" "${MAGICKCORE_LIBRARY}")
set(AVIF_MAGICK_LINK_LIBRARIES
    "${MAGICKWAND_LIBRARY}"
    "${MAGICKCORE_LIBRARY}"
    ${MAGICK_ALL_LIBRARIES})
foreach(magick_library_var IN ITEMS MAGICKWAND_LIBRARY MAGICKCORE_LIBRARY)
    file(TO_CMAKE_PATH "${${magick_library_var}}" magick_library_path)
    string(FIND "${magick_library_path}" "${AVIF_MAGICK_ROOT_CMAKE}/" magick_library_root_position)
    if(magick_library_root_position EQUAL -1)
        message(FATAL_ERROR
            "${magick_library_var} resolved outside MAGICK_ROOT. "
            "Delete the build directory or reconfigure with -U ${magick_library_var}. "
            "Resolved path: ${${magick_library_var}}")
    endif()
endforeach()

add_library(ImageMagickWandRuntime INTERFACE)
target_include_directories(ImageMagickWandRuntime BEFORE INTERFACE
    "${MAGICK_ROOT}/include/heif"
    "${MAGICK_ROOT}/include")

if(MSVC AND NOT MAGICK_RUNTIME_DLLS)
    # Static ImageMagick's delegate archives (AVIF/WebP/aom/etc.) are pulled
    # through MagickCore. Some third-party archives contain .res members, so
    # /WHOLEARCHIVE causes duplicate resource failures. Repeating the leaf
    # delegate libraries after their consumers, plus forcing a few public entry
    # symbols, keeps MSVC on the static objects instead of import members.
    set(AVIF_MAGICK_LEAF_DEPENDENCY_PATTERN
        "^CORE_(RL|DB)_(aom|brotli|de265|ffi|heif|highway|jpeg-turbo|jpeg-turbo-12|jpeg-turbo-16|webp)_$")
    set(AVIF_MAGICK_LEAF_DEPENDENCIES)
    foreach(magick_static_library IN LISTS AVIF_MAGICK_LINK_LIBRARIES)
        get_filename_component(magick_static_name "${magick_static_library}" NAME_WE)
        target_link_libraries(ImageMagickWandRuntime INTERFACE "${magick_static_library}")
        if(magick_static_name MATCHES "${AVIF_MAGICK_LEAF_DEPENDENCY_PATTERN}")
            list(APPEND AVIF_MAGICK_LEAF_DEPENDENCIES "${magick_static_library}")
        endif()
    endforeach()
    target_link_libraries(ImageMagickWandRuntime INTERFACE ${AVIF_MAGICK_LEAF_DEPENDENCIES})
    target_link_options(ImageMagickWandRuntime INTERFACE
        /NODEFAULTLIB:heif.lib
        /NODEFAULTLIB:heifd.lib
        /INCLUDE:aom_codec_av1_cx
        /INCLUDE:aom_codec_av1_dx
        /INCLUDE:aom_codec_dec_init_ver
        /INCLUDE:aom_codec_decode
        /INCLUDE:aom_codec_get_frame
        /INCLUDE:BrotliDecoderCreateInstance
        /INCLUDE:BrotliEncoderCreateInstance
        /INCLUDE:de265_new_decoder
        /INCLUDE:ffi_call
        /INCLUDE:ffi_prep_cif
        "/INCLUDE:?SupportedTargets@hwy@@YA_JXZ"
        /INCLUDE:jpeg_CreateDecompress
        /INCLUDE:jpeg_CreateCompress
        /INCLUDE:jpeg_std_error
        /INCLUDE:jpeg_read_header
        /INCLUDE:jpeg_start_decompress
        /INCLUDE:jpeg_read_scanlines
        /INCLUDE:jpeg_finish_decompress
        /INCLUDE:jpeg_mem_src
        /INCLUDE:j12init_1pass_quantizer
        /INCLUDE:j12init_2pass_quantizer
        /INCLUDE:j12init_c_coef_controller
        /INCLUDE:j12init_c_main_controller
        /INCLUDE:j12init_c_prep_controller
        /INCLUDE:j12init_color_converter
        /INCLUDE:j12init_color_deconverter
        /INCLUDE:j12init_d_coef_controller
        /INCLUDE:j12init_d_main_controller
        /INCLUDE:j12init_d_post_controller
        /INCLUDE:j12init_downsampler
        /INCLUDE:j12init_forward_dct
        /INCLUDE:j12init_inverse_dct
        /INCLUDE:j12init_merged_upsampler
        /INCLUDE:j12init_upsampler)
else()
    target_link_libraries(ImageMagickWandRuntime INTERFACE ${AVIF_MAGICK_LINK_LIBRARIES})
endif()

target_link_libraries(ImageMagickWandRuntime INTERFACE
    ws2_32
    bcrypt
    crypt32
    shlwapi)

function(avif_enable_common_options target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(${target} PUBLIC
            /utf-8
            /permissive-
            /EHsc
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Ot>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/GF>)
        target_compile_definitions(${target} PUBLIC
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/INCREMENTAL:NO>)
        endif()
    endif()
    if(AVIF_RELEASE_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()

function(avif_copy_magick_runtime target)
    file(GLOB magick_dlls CONFIGURE_DEPENDS "${MAGICK_ROOT}/*.dll")
    if(magick_dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${magick_dlls}
                    "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    else()
        foreach(stale_magick_dll
                aom.dll brotlicommon.dll brotlidec.dll brotlienc.dll
                ffi-8.dll hwy.dll jpeg62.dll libde265.dll
                CORE_RL_MagickWand_.dll CORE_RL_MagickCore_.dll
                CORE_DB_MagickWand_.dll CORE_DB_MagickCore_.dll)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -f
                        "$<TARGET_FILE_DIR:${target}>/${stale_magick_dll}"
                VERBATIM)
        endforeach()
    endif()

    foreach(config_file
            colors.xml configure.xml delegates.xml english.xml locale.xml log.xml
            mime.xml policy.xml thresholds.xml type.xml type-ghostscript.xml
            sRGB.icc License.txt NOTICE.txt)
        if(EXISTS "${MAGICK_ROOT}/${config_file}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MAGICK_ROOT}/${config_file}"
                        "$<TARGET_FILE_DIR:${target}>/${config_file}"
                VERBATIM)
        endif()
    endforeach()

    if(EXISTS "${MAGICK_ROOT}/modules")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${MAGICK_ROOT}/modules"
                    "$<TARGET_FILE_DIR:${target}>/modules"
            VERBATIM)
    endif()
endfunction()

function(avif_copy_slint_runtime target)
    if(TARGET slint_cpp-shared)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:slint_cpp-shared>"
                    "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    elseif(AVIF_STATIC_SLINT)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                    "$<TARGET_FILE_DIR:${target}>/slint_cpp.dll"
            VERBATIM)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${target}>/slint-compiler.exe"
                "$<TARGET_FILE_DIR:${target}>/slint_compiler.pdb"
        VERBATIM)
endfunction()

include(FetchContent)
if(AVIF_STATIC_SLINT)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()
set(SLINT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SLINT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_FEMTOVG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG v1.16.1
    SOURCE_SUBDIR api/cpp)
FetchContent_MakeAvailable(Slint)

function(avif_add_slint_sources target slint_file)
    get_filename_component(slint_absolute "${slint_file}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(slint_base "${slint_absolute}" NAME_WE)

    set(generated_header "${CMAKE_CURRENT_BINARY_DIR}/${slint_base}.h")
    set(generated_cpp "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_${slint_base}_1.cpp")

    # Slint's depfile currently produces noisy MSBuild warnings when the repo path
    # contains spaces. The UI has a single .slint file, so a direct dependency is
    # enough and keeps Windows paths intact.
    add_custom_command(
        OUTPUT "${generated_header}" "${generated_cpp}"
        COMMAND "$<TARGET_FILE:Slint::slint-compiler>" "${slint_absolute}"
                -f cpp
                -o "${generated_header}"
                "--cpp-file=${generated_cpp}"
        DEPENDS Slint::slint-compiler "${slint_absolute}"
        COMMENT "Generating ${slint_base}.h"
        VERBATIM)

    target_sources(${target} PRIVATE "${generated_header}" "${generated_cpp}")
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>")
endfunction()

add_library(avif_core STATIC)
avif_enable_common_options(avif_core)
target_link_libraries(avif_core PUBLIC
    scn::scn
    ImageMagickWandRuntime)
target_sources(avif_core
    PUBLIC
        FILE_SET avif_modules TYPE CXX_MODULES FILES
            src/app/config.ixx
            src/core/process.ixx
            src/backends/magick_backend.ixx
            src/app/pipeline.ixx)

add_executable(AVIF-WebP-Cli src/cli/main.cpp)
avif_enable_common_options(AVIF-WebP-Cli)
target_link_libraries(AVIF-WebP-Cli PRIVATE avif_core)
avif_copy_magick_runtime(AVIF-WebP-Cli)

add_executable(AVIF-WebP-Studio WIN32
    src/ui/main.cpp
    src/ui/AVIF-WebP-Studio.manifest)
avif_enable_common_options(AVIF-WebP-Studio)
target_link_libraries(AVIF-WebP-Studio PRIVATE
    avif_core
    Slint::Slint
    ole32
    shell32
    uxtheme
    imm32)
avif_add_slint_sources(AVIF-WebP-Studio ui/avif_studio.slint)
avif_copy_slint_runtime(AVIF-WebP-Studio)
avif_copy_magick_runtime(AVIF-WebP-Studio)
