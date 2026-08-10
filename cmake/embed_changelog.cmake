if(NOT DEFINED INPUT_ZH OR NOT DEFINED INPUT_EN OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "INPUT_ZH, INPUT_EN and OUTPUT are required")
endif()

file(READ "${INPUT_ZH}" changelog_zh)
file(READ "${INPUT_EN}" changelog_en)
string(REPLACE "\r\n" "\n" changelog_zh "${changelog_zh}")
string(REPLACE "\r" "\n" changelog_zh "${changelog_zh}")
string(REPLACE "\r\n" "\n" changelog_en "${changelog_en}")
string(REPLACE "\r" "\n" changelog_en "${changelog_en}")

set(delimiter_zh "AWJ_CHANGELOG_ZH")
set(delimiter_en "AWJ_CHANGELOG_EN")
string(FIND "${changelog_zh}" ")${delimiter_zh}\"" collision_zh)
string(FIND "${changelog_en}" ")${delimiter_en}\"" collision_en)
if(NOT collision_zh EQUAL -1 OR NOT collision_en EQUAL -1)
    message(FATAL_ERROR "CHANGELOG contains an embedded-string delimiter")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}" "#pragma once\n"
    "#include <string_view>\n\n"
    "namespace awj::embedded_changelog {\n"
    "inline constexpr std::string_view zh = R\"${delimiter_zh}(${changelog_zh})${delimiter_zh}\";\n"
    "inline constexpr std::string_view en = R\"${delimiter_en}(${changelog_en})${delimiter_en}\";\n"
    "}  // namespace awj::embedded_changelog\n")
