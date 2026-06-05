cmake_minimum_required(VERSION 3.30)

project(AWJimage
    VERSION 0.7.2
    DESCRIPTION "AWJimage native batch converter with Slint"
    LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AVIF_STATIC_SLINT "Link Slint statically so AWJ does not need slint_cpp.dll" ON)
option(AVIF_STATIC_MSVC_RUNTIME "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" ON)
option(AVIF_ENABLE_RELEASE_IPO "Enable interprocedural optimization for Release builds when the compiler supports it" OFF)
option(AWJ_ENABLE_STRICT_MSVC_CHECKS "Enable MSVC /W4 and /sdl for project targets" ON)
option(AWJ_ENABLE_X64_V3 "Enable x64-v3/AVX2 code generation for project targets in Release builds" OFF)
option(AWJ_ENABLE_ZENRAVIF "Build experimental zenrav1e into AWJ" ON)
option(AWJ_ENABLE_SVTAV1HDR "Build svt-av1-hdr into AWJ" ON)
set(AWJ_SVTAV1HDR_GIT_REPOSITORY "https://github.com/juliobbv-p/svt-av1-hdr.git" CACHE STRING "svt-av1-hdr repository")
set(AWJ_SVTAV1HDR_GIT_TAG "b90fe2474a0c464121c0724d073f01953f792034" CACHE STRING "svt-av1-hdr git tag or commit")
set(AWJ_LIBAVIF_GIT_REPOSITORY "https://github.com/AOMediaCodec/libavif.git" CACHE STRING "libavif repository used for the static SVT-enabled build")
set(AWJ_LIBAVIF_GIT_TAG "6543b22b5bc706c53f038a16fe515f921556d9b3" CACHE STRING "libavif git tag or commit")

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
if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TRIPLET)
    set(VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}" CACHE STRING "vcpkg target triplet")
else()
    set(VCPKG_TRIPLET "${AVIF_DEFAULT_VCPKG_TRIPLET}" CACHE STRING "vcpkg target triplet")
endif()
set(VCPKG_TARGET_TRIPLET "${VCPKG_TRIPLET}" CACHE STRING "vcpkg target triplet" FORCE)
if(VCPKG_ROOT AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg[.]cmake")
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

message(STATUS "AWJimage vcpkg triplet: ${VCPKG_TRIPLET}")
message(STATUS "AWJimage MSVC runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")

find_package(scn CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(GIF REQUIRED)
find_package(TIFF REQUIRED)
find_package(libraw CONFIG REQUIRED)
find_package(AOM CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)

set(AWJ_SVTAV1HDR_AVAILABLE OFF)
set(AWJ_SVTAV1HDR_STATIC_DEFINE 0)

set(AVIF_VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TRIPLET}")
function(avif_find_vcpkg_library out_var library_name)
    find_library(${out_var}_RELEASE
        NAMES ${library_name}
        PATHS "${AVIF_VCPKG_INSTALLED_DIR}/lib"
        NO_DEFAULT_PATH
        REQUIRED)
    find_library(${out_var}_DEBUG
        NAMES ${library_name}
        PATHS "${AVIF_VCPKG_INSTALLED_DIR}/debug/lib"
        NO_DEFAULT_PATH)
    if(${out_var}_DEBUG)
        set(${out_var}
            "$<$<CONFIG:Debug>:${${out_var}_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${${out_var}_RELEASE}>"
            PARENT_SCOPE)
    else()
        set(${out_var} "${${out_var}_RELEASE}" PARENT_SCOPE)
    endif()
endfunction()

find_path(JXL_INCLUDE_DIR
    NAMES jxl/encode.h
    PATHS "${AVIF_VCPKG_INSTALLED_DIR}/include"
    REQUIRED)
avif_find_vcpkg_library(JXL_LIBRARY jxl)
avif_find_vcpkg_library(JXL_THREADS_LIBRARY jxl_threads)
avif_find_vcpkg_library(JXL_CMS_LIBRARY jxl_cms)
avif_find_vcpkg_library(JXL_HIGHWAY_LIBRARY hwy)
avif_find_vcpkg_library(JXL_BROTLI_ENC_LIBRARY brotlienc)
avif_find_vcpkg_library(JXL_BROTLI_DEC_LIBRARY brotlidec)
avif_find_vcpkg_library(JXL_BROTLI_COMMON_LIBRARY brotlicommon)
avif_find_vcpkg_library(JXL_LCMS_LIBRARY lcms2)
add_library(JXL::jxl INTERFACE IMPORTED)
target_include_directories(JXL::jxl INTERFACE "${JXL_INCLUDE_DIR}")
target_compile_definitions(JXL::jxl INTERFACE
    JXL_STATIC_DEFINE
    JXL_THREADS_STATIC_DEFINE
    JXL_CMS_STATIC_DEFINE)
target_link_libraries(JXL::jxl INTERFACE
    "${JXL_LIBRARY}"
    "${JXL_THREADS_LIBRARY}"
    "${JXL_CMS_LIBRARY}"
    "${JXL_HIGHWAY_LIBRARY}"
    "${JXL_BROTLI_ENC_LIBRARY}"
    "${JXL_BROTLI_DEC_LIBRARY}"
    "${JXL_BROTLI_COMMON_LIBRARY}"
    "${JXL_LCMS_LIBRARY}")

function(avif_enable_common_options target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(${target} PUBLIC
            /utf-8
            /permissive-
            /EHsc
            $<$<BOOL:${AWJ_ENABLE_STRICT_MSVC_CHECKS}>:/W4>
            $<$<BOOL:${AWJ_ENABLE_STRICT_MSVC_CHECKS}>:/sdl>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Ot>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/GF>
            $<$<AND:$<CONFIG:Release>,$<BOOL:${AWJ_ENABLE_X64_V3}>>:/arch:AVX2>)
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

function(awj_set_runtime_directory target base_dir)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${base_dir}/${config}")
    endforeach()
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmake")
set(AWJ_WRITE_SHA256_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/write_sha256.cmake")
file(WRITE "${AWJ_WRITE_SHA256_SCRIPT}" [=[
if(NOT DEFINED AWJ_SHA256_INPUT)
    message(FATAL_ERROR "AWJ_SHA256_INPUT is required.")
endif()
if(NOT DEFINED AWJ_SHA256_OUTPUT)
    message(FATAL_ERROR "AWJ_SHA256_OUTPUT is required.")
endif()
if(NOT DEFINED AWJ_SHA256_NAME)
    get_filename_component(AWJ_SHA256_NAME "${AWJ_SHA256_INPUT}" NAME)
endif()
file(SHA256 "${AWJ_SHA256_INPUT}" AWJ_SHA256_HASH)
file(WRITE "${AWJ_SHA256_OUTPUT}" "${AWJ_SHA256_HASH}  ${AWJ_SHA256_NAME}\n")
]=])

function(awj_add_sha256_post_build target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DAWJ_SHA256_INPUT=$<TARGET_FILE:${target}>"
                "-DAWJ_SHA256_OUTPUT=$<TARGET_FILE:${target}>.sha256"
                "-DAWJ_SHA256_NAME=$<TARGET_FILE_NAME:${target}>"
                -P "${AWJ_WRITE_SHA256_SCRIPT}"
        VERBATIM)
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

set(AWJ_VISUAL_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/core/shaders")
set(AWJ_VISUAL_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/visual_shaders")
set(AWJ_VISUAL_SHADER_HEADER "${AWJ_VISUAL_SHADER_GENERATED_DIR}/awj_visual_metric_shaders.hpp")
file(MAKE_DIRECTORY "${AWJ_VISUAL_SHADER_GENERATED_DIR}")

set(AWJ_SHADER_COMPILER_DIRS)
if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    list(APPEND AWJ_SHADER_COMPILER_DIRS "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64")
endif()
set(AWJ_WINDOWS_KIT_ROOT_CANDIDATES)
if(DEFINED ENV{ProgramFiles})
    list(APPEND AWJ_WINDOWS_KIT_ROOT_CANDIDATES "$ENV{ProgramFiles}/Windows Kits/10")
endif()
list(APPEND AWJ_WINDOWS_KIT_ROOT_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10"
    "C:/Program Files/Windows Kits/10")
foreach(AWJ_WINDOWS_KIT_ROOT IN LISTS AWJ_WINDOWS_KIT_ROOT_CANDIDATES)
    file(GLOB AWJ_WINDOWS_KIT_SHADER_DIRS "${AWJ_WINDOWS_KIT_ROOT}/bin/*/x64")
    list(SORT AWJ_WINDOWS_KIT_SHADER_DIRS)
    list(REVERSE AWJ_WINDOWS_KIT_SHADER_DIRS)
    list(APPEND AWJ_SHADER_COMPILER_DIRS ${AWJ_WINDOWS_KIT_SHADER_DIRS})
endforeach()
find_program(AWJ_FXC_EXECUTABLE NAMES fxc.exe fxc PATHS ${AWJ_SHADER_COMPILER_DIRS} NO_DEFAULT_PATH)
if(NOT AWJ_FXC_EXECUTABLE)
    find_program(AWJ_FXC_EXECUTABLE NAMES fxc.exe fxc)
endif()
if(NOT AWJ_FXC_EXECUTABLE)
    message(FATAL_ERROR "找不到 fxc.exe，无法预编译 visual_quality Direct3D 11 shader。请安装 Windows SDK；最终用户运行时不需要该工具。")
endif()
message(STATUS "AWJimage visual metric shader compiler: ${AWJ_FXC_EXECUTABLE}")

function(awj_compile_visual_shader out_var shader_name hlsl_name)
    set(output_path "${AWJ_VISUAL_SHADER_GENERATED_DIR}/${shader_name}.cso")
    add_custom_command(
        OUTPUT "${output_path}"
        COMMAND "${AWJ_FXC_EXECUTABLE}" /nologo /T cs_5_0 /E main /Ges /O3
                /Fo "${output_path}"
                "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
        DEPENDS "${AWJ_VISUAL_SHADER_DIR}/${hlsl_name}"
        COMMENT "Precompiling visual metric shader ${hlsl_name}"
        VERBATIM)
    set(${out_var} "${output_path}" PARENT_SCOPE)
endfunction()

awj_compile_visual_shader(AWJ_LUMA_CSO luma visual_luma.hlsl)
awj_compile_visual_shader(AWJ_GMSD_CSO gmsd visual_gmsd.hlsl)
awj_compile_visual_shader(AWJ_DOWNSAMPLE_CSO downsample visual_downsample.hlsl)
awj_compile_visual_shader(AWJ_MS_SSIM_CSO ms_ssim visual_ms_ssim.hlsl)

add_custom_command(
    OUTPUT "${AWJ_VISUAL_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            "-DAWJ_SHADER_HEADER=${AWJ_VISUAL_SHADER_HEADER}"
            "-DAWJ_LUMA_CSO=${AWJ_LUMA_CSO}"
            "-DAWJ_GMSD_CSO=${AWJ_GMSD_CSO}"
            "-DAWJ_DOWNSAMPLE_CSO=${AWJ_DOWNSAMPLE_CSO}"
            "-DAWJ_MS_SSIM_CSO=${AWJ_MS_SSIM_CSO}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_visual_shader_header.cmake"
    DEPENDS "${AWJ_LUMA_CSO}" "${AWJ_GMSD_CSO}" "${AWJ_DOWNSAMPLE_CSO}" "${AWJ_MS_SSIM_CSO}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_visual_shader_header.cmake"
    COMMENT "Embedding visual metric shader bytecode"
    VERBATIM)
add_custom_target(awj_visual_metric_shaders DEPENDS "${AWJ_VISUAL_SHADER_HEADER}")

include(FetchContent)

if(AWJ_ENABLE_SVTAV1HDR)
    set(AWJ_PARENT_BUILD_TESTING ${BUILD_TESTING})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_DEC OFF CACHE BOOL "" FORCE)
    set(COMPILE_C_ONLY ON CACHE BOOL "" FORCE)
    set(ENABLE_AVX512 OFF CACHE BOOL "" FORCE)
    set(SVT_AV1_BUILD_APPS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SvtAv1Hdr
        GIT_REPOSITORY ${AWJ_SVTAV1HDR_GIT_REPOSITORY}
        GIT_TAG ${AWJ_SVTAV1HDR_GIT_TAG})
    FetchContent_MakeAvailable(SvtAv1Hdr)
    if(TARGET SvtAv1Enc)
        set(AWJ_SVTAV1HDR_AVAILABLE ON)
        set(AWJ_SVTAV1HDR_STATIC_DEFINE 1)
        set(AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR "${CMAKE_CURRENT_BINARY_DIR}/svtav1hdr-include/svt-av1")
        file(MAKE_DIRECTORY "${AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR}")
        file(GLOB AWJ_SVTAV1HDR_API_HEADERS "${svtav1hdr_SOURCE_DIR}/Source/API/*.h")
        file(COPY ${AWJ_SVTAV1HDR_API_HEADERS} DESTINATION "${AWJ_SVTAV1HDR_INCLUDE_COMPAT_DIR}")
        target_include_directories(SvtAv1Enc INTERFACE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/svtav1hdr-include>")
    else()
        message(FATAL_ERROR "svt-av1-hdr FetchContent did not provide SvtAv1Enc.")
    endif()
    set(BUILD_TESTING ${AWJ_PARENT_BUILD_TESTING} CACHE BOOL "" FORCE)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM SYSTEM CACHE STRING "" FORCE)
set(AVIF_CODEC_AOM_DECODE ON CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM_ENCODE ON CACHE BOOL "" FORCE)
if(AWJ_SVTAV1HDR_AVAILABLE)
    set(AVIF_CODEC_SVT SYSTEM CACHE STRING "" FORCE)
else()
    set(AVIF_CODEC_SVT OFF CACHE STRING "" FORCE)
endif()
set(AVIF_LIBYUV SYSTEM CACHE STRING "" FORCE)
set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AVIF_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libavif
    GIT_REPOSITORY ${AWJ_LIBAVIF_GIT_REPOSITORY}
    GIT_TAG ${AWJ_LIBAVIF_GIT_TAG})
FetchContent_MakeAvailable(libavif)

if(AVIF_STATIC_SLINT)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()
set(SLINT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SLINT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "" FORCE)
FetchContent_Declare(Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG e9c1ca295f9356af71f1e251c287de18406b46f6
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

add_library(awj_base STATIC)
avif_enable_common_options(awj_base)
add_dependencies(awj_base awj_visual_metric_shaders)
target_link_libraries(awj_base PUBLIC
    scn::scn
    bcrypt
    d3d11)
target_sources(awj_base PRIVATE "${AWJ_VISUAL_SHADER_HEADER}")
target_include_directories(awj_base PUBLIC "$<BUILD_INTERFACE:${AWJ_VISUAL_SHADER_GENERATED_DIR}>")
target_sources(awj_base
    PUBLIC
        FILE_SET awj_base_modules TYPE CXX_MODULES FILES
            src/core/encoding_defaults.ixx
            src/core/large_image_plan.ixx
            src/app/config.ixx
            src/core/visual_quality.ixx
            src/core/visual_metrics.ixx
            src/core/visual_metrics_gpu.ixx
            src/core/image.ixx
            src/core/resource_planner.ixx
            src/core/codec.ixx
            src/core/process.ixx)

add_library(awj_raw_image_io STATIC)
avif_enable_common_options(awj_raw_image_io)
target_link_libraries(awj_raw_image_io PUBLIC awj_base)
target_sources(awj_raw_image_io
    PUBLIC
        FILE_SET awj_raw_image_io_modules TYPE CXX_MODULES FILES
            src/core/raw_image_io.ixx)

add_library(awj_native_visual_search STATIC)
avif_enable_common_options(awj_native_visual_search)
target_link_libraries(awj_native_visual_search PUBLIC awj_base)
target_sources(awj_native_visual_search
    PUBLIC
        FILE_SET awj_native_visual_search_modules TYPE CXX_MODULES FILES
            src/core/native_visual_search.ixx)

add_library(awj_native_codecs STATIC)
avif_enable_common_options(awj_native_codecs)
target_link_libraries(awj_native_codecs PUBLIC
    awj_base
    awj_raw_image_io
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux
    JXL::jxl
    PNG::PNG
    JPEG::JPEG
    GIF::GIF
    TIFF::TIFF
    libraw::raw_r
    windowscodecs)
target_sources(awj_native_codecs
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_registry.ixx
            src/codecs/decoder_common.ixx
            src/codecs/webp_codec.ixx
            src/codecs/jxl_codec.ixx
            src/codecs/png_codec.ixx
            src/codecs/jpeg_codec.ixx
            src/codecs/gif_codec.ixx
            src/codecs/tiff_codec.ixx
            src/codecs/libraw_codec.ixx
            src/codecs/raw_codec.ixx
            src/codecs/wic_codec.ixx)

set(ZENRAVIF_BRIDGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/Cargo.toml")
set(ZENRAVIF_BRIDGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/src/lib.rs")
set(AWJ_ZENRAVIF_AVAILABLE OFF)
if(AWJ_ENABLE_ZENRAVIF AND EXISTS "${ZENRAVIF_BRIDGE_MANIFEST}" AND EXISTS "${ZENRAVIF_BRIDGE_SOURCE}")
    set(AWJ_ZENRAVIF_AVAILABLE ON)
    set(ZENRAVIF_BRIDGE_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zenravif-bridge/target")
    set(ZENRAVIF_BRIDGE_LIB "${ZENRAVIF_BRIDGE_TARGET_DIR}/release/zenravif_bridge.lib")
    add_custom_command(
        OUTPUT "${ZENRAVIF_BRIDGE_LIB}"
        COMMAND cargo build --manifest-path "${ZENRAVIF_BRIDGE_MANIFEST}" --release
        DEPENDS
            "${ZENRAVIF_BRIDGE_MANIFEST}"
            "${ZENRAVIF_BRIDGE_SOURCE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building zenravif Rust static library"
        VERBATIM)
    add_custom_target(zenravif_bridge_static DEPENDS "${ZENRAVIF_BRIDGE_LIB}")
    add_library(zenravif_bridge STATIC IMPORTED GLOBAL)
    set_target_properties(zenravif_bridge PROPERTIES
        IMPORTED_LOCATION "${ZENRAVIF_BRIDGE_LIB}"
        INTERFACE_LINK_LIBRARIES "ws2_32;userenv;ntdll;bcrypt;advapi32")
    add_dependencies(zenravif_bridge zenravif_bridge_static)
elseif(AWJ_ENABLE_ZENRAVIF)
    message(STATUS "zenravif bridge sources were not found; experimental zenrav1e AVIF encoder will be unavailable in this build.")
else()
    message(STATUS "Experimental zenrav1e AVIF encoder disabled by AWJ_ENABLE_ZENRAVIF=OFF.")
endif()

add_library(awj_avif_base_codec STATIC)
avif_enable_common_options(awj_avif_base_codec)
target_link_libraries(awj_avif_base_codec PUBLIC
    awj_native_codecs
    avif)
if(AWJ_ZENRAVIF_AVAILABLE)
    target_link_libraries(awj_avif_base_codec PUBLIC zenravif_bridge)
    target_compile_definitions(awj_avif_base_codec PUBLIC AWJ_HAS_ZENRAVIF=1)
    add_dependencies(awj_avif_base_codec zenravif_bridge_static)
else()
    target_compile_definitions(awj_avif_base_codec PUBLIC AWJ_HAS_ZENRAVIF=0)
endif()
target_compile_definitions(awj_avif_base_codec PUBLIC
    AWJ_HAS_SVTAV1HDR_STATIC=${AWJ_SVTAV1HDR_STATIC_DEFINE})
target_sources(awj_avif_base_codec
    PUBLIC
        FILE_SET avif_base_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_aom_codec.ixx)

add_library(awj_svtav1hdr_codec STATIC)
avif_enable_common_options(awj_svtav1hdr_codec)
target_link_libraries(awj_svtav1hdr_codec PUBLIC
    awj_base
    awj_native_codecs
    avif)
target_compile_definitions(awj_svtav1hdr_codec PUBLIC
    AWJ_HAS_SVTAV1HDR_STATIC=${AWJ_SVTAV1HDR_STATIC_DEFINE})
if(AWJ_SVTAV1HDR_AVAILABLE)
    target_link_libraries(awj_svtav1hdr_codec PUBLIC SvtAv1Enc)
endif()
target_sources(awj_svtav1hdr_codec
    PUBLIC
        FILE_SET awj_svtav1hdr_codec_modules TYPE CXX_MODULES FILES
            src/codecs/svtav1hdr_codec.ixx)

add_library(awj_native_backend STATIC)
avif_enable_common_options(awj_native_backend)
target_link_libraries(awj_native_backend PUBLIC
    awj_base
    awj_raw_image_io
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec
    awj_svtav1hdr_codec)
add_dependencies(awj_native_backend
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec
    awj_svtav1hdr_codec)
target_sources(awj_native_backend
    PUBLIC
        FILE_SET awj_native_backend_modules TYPE CXX_MODULES FILES
            src/codecs/decoder_registry.ixx
            src/backends/native_backend.ixx)

add_library(awj_pipeline STATIC)
avif_enable_common_options(awj_pipeline)
target_link_libraries(awj_pipeline PUBLIC
    awj_base
    awj_native_backend)
target_sources(awj_pipeline
    PUBLIC
        FILE_SET awj_pipeline_modules TYPE CXX_MODULES FILES
            src/app/pipeline.ixx)

add_library(awj_core INTERFACE)
target_link_libraries(awj_core INTERFACE awj_pipeline)

if(BUILD_TESTING)
    add_executable(awj_visual_quality_tests tests/visual_quality_tests.cpp)
    avif_enable_common_options(awj_visual_quality_tests)
    target_link_libraries(awj_visual_quality_tests PRIVATE awj_base)

    add_executable(awj_config_defaults_tests tests/config_defaults_tests.cpp)
    avif_enable_common_options(awj_config_defaults_tests)
    target_link_libraries(awj_config_defaults_tests PRIVATE awj_base)

    add_executable(awj_resource_planner_tests tests/resource_planner_tests.cpp)
    avif_enable_common_options(awj_resource_planner_tests)
    target_link_libraries(awj_resource_planner_tests PRIVATE awj_base)

    add_executable(awj_visual_metrics_tests tests/visual_metrics_tests.cpp)
    avif_enable_common_options(awj_visual_metrics_tests)
    target_link_libraries(awj_visual_metrics_tests PRIVATE awj_base)

    add_executable(awj_webp_codec_tests tests/webp_codec_tests.cpp)
    avif_enable_common_options(awj_webp_codec_tests)
    target_link_libraries(awj_webp_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_avif_aom_codec_tests tests/avif_aom_codec_tests.cpp)
    avif_enable_common_options(awj_avif_aom_codec_tests)
    target_link_libraries(awj_avif_aom_codec_tests PRIVATE awj_avif_base_codec)

    add_executable(awj_avif_registry_tests tests/avif_registry_tests.cpp)
    avif_enable_common_options(awj_avif_registry_tests)
    target_link_libraries(awj_avif_registry_tests PRIVATE awj_native_codecs)

    add_executable(awj_jxl_codec_tests tests/jxl_codec_tests.cpp)
    avif_enable_common_options(awj_jxl_codec_tests)
    target_link_libraries(awj_jxl_codec_tests PRIVATE awj_native_codecs)

    add_executable(awj_native_visual_search_tests tests/native_visual_search_tests.cpp)
    avif_enable_common_options(awj_native_visual_search_tests)
    target_link_libraries(awj_native_visual_search_tests PRIVATE awj_native_visual_search awj_native_codecs)

    add_executable(awj_decoder_registry_tests tests/decoder_registry_tests.cpp)
    avif_enable_common_options(awj_decoder_registry_tests)
    target_link_libraries(awj_decoder_registry_tests PRIVATE awj_native_backend)

    add_executable(awj_native_pipeline_tests tests/native_pipeline_tests.cpp)
    avif_enable_common_options(awj_native_pipeline_tests)
    target_link_libraries(awj_native_pipeline_tests PRIVATE awj_native_backend)

    add_executable(awj_pipeline_security_tests tests/pipeline_security_tests.cpp)
    avif_enable_common_options(awj_pipeline_security_tests)
    target_link_libraries(awj_pipeline_security_tests PRIVATE awj_pipeline awj_native_codecs)

    add_executable(awj_memory_cancel_tests tests/memory_cancel_tests.cpp)
    avif_enable_common_options(awj_memory_cancel_tests)
    target_link_libraries(awj_memory_cancel_tests PRIVATE awj_native_visual_search awj_native_codecs awj_raw_image_io)

    add_executable(awj_cli_config_entry tests/cli_config_entry.cpp)
    avif_enable_common_options(awj_cli_config_entry)
    target_link_libraries(awj_cli_config_entry PRIVATE awj_base)

    add_executable(awj_cli_native_avif_aom_tests tests/cli_native_avif_aom_tests.cpp)
    avif_enable_common_options(awj_cli_native_avif_aom_tests)
    target_link_libraries(awj_cli_native_avif_aom_tests PRIVATE awj_native_backend)

    add_test(NAME visual_quality_core COMMAND awj_visual_quality_tests)
    add_test(NAME config_defaults_core COMMAND awj_config_defaults_tests)
    add_test(NAME resource_planner_core COMMAND awj_resource_planner_tests)
    add_test(NAME visual_metrics_core COMMAND awj_visual_metrics_tests)
    add_test(NAME webp_codec_core COMMAND awj_webp_codec_tests)
    add_test(NAME avif_aom_codec_core COMMAND awj_avif_aom_codec_tests)
    add_test(NAME avif_registry_core COMMAND awj_avif_registry_tests)
    add_test(NAME jxl_codec_core COMMAND awj_jxl_codec_tests)
    add_test(NAME native_visual_search_core COMMAND awj_native_visual_search_tests)
    add_test(NAME decoder_registry_core COMMAND awj_decoder_registry_tests)
    add_test(NAME native_pipeline_core COMMAND awj_native_pipeline_tests)
    add_test(NAME pipeline_security_core COMMAND awj_pipeline_security_tests)
    add_test(NAME memory_cancel_core COMMAND awj_memory_cancel_tests)

    add_test(NAME cli_help COMMAND awj_cli_config_entry --help)
    set_tests_properties(cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "AWJimage C[+][+]23")

    add_test(NAME cli_unknown_argument
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--definitely-unknown-option' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('未知参数')) { exit 1 }")

    add_test(NAME cli_rejects_nan_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--quality' 'nan' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('质量参数必须是有限数字')) { exit 1 }")

    add_test(NAME cli_rejects_low_visual_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '0' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_rejects_high_visual_quality
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--visual-quality' '101' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('visual-quality 范围必须在 1 到 100 之间')) { exit 1 }")

    add_test(NAME cli_accepts_visual_quality_priority
        COMMAND awj_cli_config_entry --visual-quality 90 --quality 20 --help)

    add_test(NAME cli_native_avif_aom_converts_webp
        COMMAND awj_cli_native_avif_aom_tests)

    add_test(NAME cli_rejects_svt_444
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'svt' '--chroma' '444' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('only supports 420')) { exit 1 }")

    add_test(NAME cli_rejects_rav1e
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--avif-encoder' 'rav1e' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('AVIF encoder 不支持')) { exit 1 }")

    add_test(NAME cli_rejects_tiny_memory_limit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--memory-limit' '1MiB' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('memory-limit 不能低于 64MiB')) { exit 1 }")

    add_test(NAME cli_accepts_auto_memory_limit
        COMMAND awj_cli_config_entry --memory-limit auto --help)

    add_test(NAME cli_rejects_empty_option_key
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--option' '=x' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('--option 的 key 不能为空')) { exit 1 }")
endif()

add_executable(AWJ WIN32
    src/app/main.cpp
    src/cli/main.cpp
    src/ui/main.cpp
    src/ui/AWJ.manifest
    src/ui/AWJ.rc)
avif_enable_common_options(AWJ)
target_compile_definitions(AWJ PRIVATE AWJ_UNIFIED_EXE)
if(MSVC)
    target_link_options(AWJ PRIVATE "/ENTRY:wmainCRTStartup")
endif()
target_link_libraries(AWJ PRIVATE
    awj_core
    Slint::Slint
    dwmapi
    ole32
    shell32
    uxtheme
    imm32
    opengl32)
if(MSVC AND AVIF_STATIC_SLINT AND AWJ_ZENRAVIF_AVAILABLE)
    # Both Slint's static C++ runtime and zenravif_bridge are Rust static libraries.
    # MSVC otherwise reports duplicate Rust std runtime symbols even when built by
    # the same toolchain for one final executable.
    target_link_options(AWJ PRIVATE "/FORCE:MULTIPLE")
endif()
avif_add_slint_sources(AWJ ui/awj_studio.slint)
avif_copy_slint_runtime(AWJ)
awj_add_sha256_post_build(AWJ)

add_executable(AWJ-com src/cli/shim_main.cpp)
avif_enable_common_options(AWJ-com)
set_target_properties(AWJ-com PROPERTIES
    OUTPUT_NAME "AWJ"
    SUFFIX ".com"
    PDB_NAME "AWJ-com")
awj_add_sha256_post_build(AWJ-com)
