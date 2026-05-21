cmake_minimum_required(VERSION 3.30)

project(AWJimage
    VERSION 0.3.1
    DESCRIPTION "AWJimage native batch converter with Slint"
    LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AVIF_STATIC_SLINT "Link Slint statically so AWJ-studio does not need slint_cpp.dll" ON)
option(AVIF_STATIC_MSVC_RUNTIME "Use /MT or /MTd for smaller redistribution bundles when all dependencies allow it" ON)
option(AVIF_ENABLE_RELEASE_IPO "Enable interprocedural optimization for Release builds when the compiler supports it" OFF)
option(AWJ_ENABLE_ZENRAVIF "Build experimental zenrav1e into the private AVIF helper" ON)

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
find_package(libavif CONFIG REQUIRED)

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

function(awj_set_runtime_directory target base_dir)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${base_dir}/${config}")
    endforeach()
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
target_link_libraries(awj_base PUBLIC
    scn::scn)
target_sources(awj_base
    PUBLIC
        FILE_SET awj_base_modules TYPE CXX_MODULES FILES
            src/core/encoding_defaults.ixx
            src/app/config.ixx
            src/core/visual_quality.ixx
            src/core/visual_metrics.ixx
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
    WebP::webp
    WebP::webpdemux
    WebP::libwebpmux
    JXL::jxl)
target_sources(awj_native_codecs
    PUBLIC
        FILE_SET avif_native_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_registry.ixx
            src/codecs/webp_codec.ixx
            src/codecs/jxl_codec.ixx)

add_library(awj_avif_base_codec STATIC)
avif_enable_common_options(awj_avif_base_codec)
target_link_libraries(awj_avif_base_codec PUBLIC
    awj_native_codecs
    avif)
target_compile_definitions(awj_avif_base_codec PUBLIC AWJ_HAS_ZENRAVIF=0)
target_sources(awj_avif_base_codec
    PUBLIC
        FILE_SET avif_base_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_aom_codec.ixx)

add_library(awj_avif_helper_codec STATIC)
avif_enable_common_options(awj_avif_helper_codec)
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

target_link_libraries(awj_avif_helper_codec PUBLIC
    awj_native_codecs
    avif)
if(AWJ_ZENRAVIF_AVAILABLE)
    target_link_libraries(awj_avif_helper_codec PUBLIC zenravif_bridge)
    target_compile_definitions(awj_avif_helper_codec PUBLIC AWJ_HAS_ZENRAVIF=1)
    add_dependencies(awj_avif_helper_codec zenravif_bridge_static)
else()
    target_compile_definitions(awj_avif_helper_codec PUBLIC AWJ_HAS_ZENRAVIF=0)
endif()
target_sources(awj_avif_helper_codec
    PUBLIC
        FILE_SET avif_helper_codec_modules TYPE CXX_MODULES FILES
            src/codecs/avif_aom_codec.ixx)

add_executable(AWJ-native-avif-helper src/cli/native_avif_aom_helper.cpp)
avif_enable_common_options(AWJ-native-avif-helper)
awj_set_runtime_directory(AWJ-native-avif-helper "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/internal")
target_link_libraries(AWJ-native-avif-helper PRIVATE awj_avif_helper_codec awj_raw_image_io)

add_library(awj_native_backend STATIC)
avif_enable_common_options(awj_native_backend)
target_link_libraries(awj_native_backend PUBLIC
    awj_base
    awj_raw_image_io
    awj_native_visual_search
    awj_native_codecs
    awj_avif_base_codec)
target_sources(awj_native_backend
    PUBLIC
        FILE_SET awj_native_backend_modules TYPE CXX_MODULES FILES
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

add_executable(AWJ-cli src/cli/main.cpp)
avif_enable_common_options(AWJ-cli)
target_link_libraries(AWJ-cli PRIVATE awj_core)
add_dependencies(AWJ-cli AWJ-native-avif-helper)

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

    add_executable(awj_native_pipeline_tests tests/native_pipeline_tests.cpp)
    avif_enable_common_options(awj_native_pipeline_tests)
    target_link_libraries(awj_native_pipeline_tests PRIVATE awj_native_backend)
    add_dependencies(awj_native_pipeline_tests AWJ-native-avif-helper)

    add_executable(awj_cli_config_entry tests/cli_config_entry.cpp)
    avif_enable_common_options(awj_cli_config_entry)
    target_link_libraries(awj_cli_config_entry PRIVATE awj_base)

    add_executable(awj_cli_native_avif_aom_tests tests/cli_native_avif_aom_tests.cpp)
    avif_enable_common_options(awj_cli_native_avif_aom_tests)
    target_link_libraries(awj_cli_native_avif_aom_tests PRIVATE awj_native_backend)
    add_dependencies(awj_cli_native_avif_aom_tests AWJ-native-avif-helper)

    add_test(NAME visual_quality_core COMMAND awj_visual_quality_tests)
    add_test(NAME config_defaults_core COMMAND awj_config_defaults_tests)
    add_test(NAME resource_planner_core COMMAND awj_resource_planner_tests)
    add_test(NAME visual_metrics_core COMMAND awj_visual_metrics_tests)
    add_test(NAME webp_codec_core COMMAND awj_webp_codec_tests)
    add_test(NAME avif_aom_codec_core COMMAND awj_avif_aom_codec_tests)
    add_test(NAME avif_registry_core COMMAND awj_avif_registry_tests)
    add_test(NAME jxl_codec_core COMMAND awj_jxl_codec_tests)
    add_test(NAME native_visual_search_core COMMAND awj_native_visual_search_tests)
    add_test(NAME native_pipeline_core COMMAND awj_native_pipeline_tests)

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

    add_test(NAME cli_accepts_backend_native_webp
        COMMAND awj_cli_config_entry --backend native --format webp --help)

    add_test(NAME cli_rejects_backend_magick
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$OutputEncoding = [System.Text.UTF8Encoding]::new(); [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); $output = & '$<TARGET_FILE:awj_cli_config_entry>' '--backend' 'magick' 2>&1; $code = $LASTEXITCODE; $output; if ($code -eq 0) { exit 1 }; if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape('Magick 后端已移除')) { exit 1 }")

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

add_executable(AWJ-studio WIN32
    src/ui/main.cpp
    src/ui/AWJ-studio.manifest)
avif_enable_common_options(AWJ-studio)
target_link_libraries(AWJ-studio PRIVATE
    awj_core
    Slint::Slint
    ole32
    shell32
    uxtheme
    imm32
    opengl32)
add_dependencies(AWJ-studio AWJ-native-avif-helper)
avif_add_slint_sources(AWJ-studio ui/awj_studio.slint)
avif_copy_slint_runtime(AWJ-studio)
