if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()

set(CONFIG_H "${SOURCE_DIR}/api/cpp/include/private/slint_config.h")
if(NOT EXISTS "${CONFIG_H}")
    message(FATAL_ERROR "Slint config header not found: ${CONFIG_H}")
endif()

file(READ "${CONFIG_H}" CONTENT)
set(NEEDLE "#if !defined(DOXYGEN)\n#    if defined(_MSC_VER)")
set(REPLACEMENT "#if !defined(DOXYGEN)\n#    if defined(SLINT_STATIC)\n#        define SLINT_DLL_IMPORT\n#    elif defined(_MSC_VER)")

if(NOT CONTENT MATCHES "defined\\(SLINT_STATIC\\)")
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" PATCHED "${CONTENT}")
    if(PATCHED STREQUAL CONTENT)
        message(FATAL_ERROR "Could not patch Slint static import declarations.")
    endif()
    file(WRITE "${CONFIG_H}" "${PATCHED}")
endif()

file(TOUCH "${SOURCE_DIR}/api/cpp/build.rs")
