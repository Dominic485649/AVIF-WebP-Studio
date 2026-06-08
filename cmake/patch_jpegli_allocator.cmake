if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required.")
endif()

set(COMMON_H "${SOURCE_DIR}/lib/base/common.h")
if(NOT EXISTS "${COMMON_H}")
    message(FATAL_ERROR "Jpegli common.h not found: ${COMMON_H}")
endif()

file(READ "${COMMON_H}" CONTENT)
set(NEEDLE "  using value_type = T;\n  template <typename U>\n  struct rebind {")
set(REPLACEMENT "  using value_type = T;\n  UninitializedAllocator() = default;\n  template <typename U>\n  UninitializedAllocator(const UninitializedAllocator<U>&) noexcept {}\n  template <typename U>\n  struct rebind {")

if(CONTENT MATCHES "UninitializedAllocator\\(const UninitializedAllocator<U>&\\)")
    return()
endif()

string(REPLACE "${NEEDLE}" "${REPLACEMENT}" PATCHED "${CONTENT}")
if(PATCHED STREQUAL CONTENT)
    message(FATAL_ERROR "Could not patch Jpegli UninitializedAllocator.")
endif()

file(WRITE "${COMMON_H}" "${PATCHED}")
