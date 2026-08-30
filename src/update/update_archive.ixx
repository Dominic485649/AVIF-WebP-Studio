module;

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module awj.update_archive;

import awj.update_manifest_v2;
import awj.update_model;
import awj.update_runtime;

export namespace awj::update {

namespace archive_detail {

namespace fs = std::filesystem;

struct ArchiveCloser {
  void operator()(archive* value) const noexcept {
    if (value != nullptr) archive_read_free(value);
  }
};

using UniqueArchive = std::unique_ptr<archive, ArchiveCloser>;

std::string archive_error(archive* value, std::string_view prefix) {
  const auto* detail = value == nullptr ? nullptr : archive_error_string(value);
  return detail == nullptr ? std::string{prefix}
                           : std::format("{}：{}", prefix, detail);
}

std::string ascii_fold(std::string_view value) {
  std::string out{value};
  std::ranges::transform(out, out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

std::expected<std::size_t, std::string> expected_member_index(
    const ArchiveAssetInfo& expected, std::string_view name) {
  const auto folded = ascii_fold(name);
  for (std::size_t index = 0; index < expected.members.size(); ++index) {
    if (ascii_fold(expected.members[index].path) == folded) return index;
  }
  return std::unexpected{"归档包含未在签名 manifest 中列出的成员。"};
}

bool expected_directory_is_implied(const ArchiveAssetInfo& expected,
                                   std::string_view directory) {
  const auto prefix = ascii_fold(directory) + "/";
  return std::ranges::any_of(expected.members, [&](const ArchiveMemberInfo& member) {
    return ascii_fold(member.path).starts_with(prefix);
  });
}

}  // namespace archive_detail

bool archive_member_path_is_safe(std::string_view path) noexcept {
  if (path.empty() || path.size() > 512 || path.front() == '/' ||
      path.contains('\\') || path.contains(':') || path.contains('\0')) {
    return false;
  }
  std::size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto component = path.substr(
        start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (component.empty() || component == "." || component == ".." ||
        std::ranges::any_of(component, [](unsigned char ch) {
          return ch < 0x20 || ch == 0x7f;
        })) {
      return false;
    }
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}

const ArchiveMemberInfo* find_archive_member(const ArchiveAssetInfo& asset,
                                             std::string_view path) noexcept {
  const auto it = std::ranges::find(asset.members, path, &ArchiveMemberInfo::path);
  return it == asset.members.end() ? nullptr : &*it;
}

std::expected<void, std::string> extract_verified_7z_archive(
    const std::filesystem::path& archive_path, const ArchiveAssetInfo& expected,
    const std::filesystem::path& destination) {
  if (expected.members.empty()) {
    return std::unexpected{"签名 manifest 未声明归档成员。"};
  }
  if (auto valid = verify_asset_file(archive_path, expected.archive); !valid) {
    return valid;
  }
  std::error_code ec;
  if (!std::filesystem::create_directory(destination, ec) || ec) {
    return std::unexpected{"无法创建全新的归档提取 staging 目录。"};
  }
  archive_detail::UniqueArchive input{archive_read_new()};
  if (!input || archive_read_support_filter_all(input.get()) != ARCHIVE_OK ||
      archive_read_support_format_7zip(input.get()) != ARCHIVE_OK) {
    return std::unexpected{archive_detail::archive_error(input.get(), "无法打开 7z 更新归档")};
  }
#ifdef _WIN32
  const auto opened =
      archive_read_open_filename_w(input.get(), archive_path.c_str(), 64 * 1024);
#else
  const auto opened =
      archive_read_open_filename(input.get(), archive_path.c_str(), 64 * 1024);
#endif
  if (opened != ARCHIVE_OK) {
    return std::unexpected{archive_detail::archive_error(input.get(), "无法打开 7z 更新归档")};
  }

  std::vector<bool> seen(expected.members.size());
  bool saw_entry = false;
  archive_entry* entry = nullptr;
  for (;;) {
    const auto result = archive_read_next_header(input.get(), &entry);
    if (result == ARCHIVE_EOF) break;
    if (result != ARCHIVE_OK) {
      return std::unexpected{
          archive_detail::archive_error(input.get(), "读取 7z 更新归档失败")};
    }
    saw_entry = true;
    if (archive_format(input.get()) != ARCHIVE_FORMAT_7ZIP) {
      return std::unexpected{"更新归档不是受支持的 7z 格式。"};
    }
    const char* raw_name = archive_entry_pathname(entry);
    if (raw_name == nullptr) {
      return std::unexpected{"更新归档成员缺少路径。"};
    }
    const std::string name{raw_name};
    const auto filetype = archive_entry_filetype(entry);
    if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
      return std::unexpected{"更新归档拒绝链接成员。"};
    }
    if (filetype == AE_IFDIR) {
      std::string directory{name};
      if (directory.ends_with('/')) directory.pop_back();
      if (!archive_member_path_is_safe(directory) ||
          !archive_detail::expected_directory_is_implied(expected, directory)) {
        return std::unexpected{"更新归档包含未由签名成员路径声明的目录。"};
      }
      if (archive_read_data_skip(input.get()) != ARCHIVE_OK) {
        return std::unexpected{
            archive_detail::archive_error(input.get(), "跳过 7z 目录成员失败")};
      }
      continue;
    }
    if (!archive_member_path_is_safe(name)) {
      return std::unexpected{"更新归档成员路径不安全。"};
    }
    auto member_index = archive_detail::expected_member_index(expected, name);
    if (!member_index) return std::unexpected{member_index.error()};
    if (seen[*member_index]) {
      return std::unexpected{"更新归档含重复或仅大小写不同的成员。"};
    }
    if (filetype != AE_IFREG) {
      return std::unexpected{"更新归档只允许普通文件和已签名路径隐含的目录。"};
    }
    const auto declared_size = archive_entry_size(entry);
    const auto expected_size = expected.members[*member_index].asset.size_bytes;
    if (declared_size < 0 || static_cast<std::uint64_t>(declared_size) != expected_size) {
      return std::unexpected{"更新归档成员大小与签名 manifest 不一致。"};
    }

    const std::u8string utf8_name{
        reinterpret_cast<const char8_t*>(name.data()), name.size()};
    const std::filesystem::path output = destination / std::filesystem::path{utf8_name};
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec || std::filesystem::exists(output, ec) || ec) {
      return std::unexpected{"更新归档成员输出路径不可安全创建。"};
    }
    std::ofstream stream{output, std::ios::binary | std::ios::out};
    if (!stream) return std::unexpected{"无法写入更新归档成员。"};
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
      const auto count = archive_read_data(input.get(), buffer.data(), buffer.size());
      if (count == 0) break;
      if (count < 0 || total > expected_size - static_cast<std::uint64_t>(count)) {
        return std::unexpected{"更新归档成员解压失败或超过签名大小限制。"};
      }
      stream.write(buffer.data(), count);
      if (!stream) return std::unexpected{"写入更新归档成员失败。"};
      total += static_cast<std::uint64_t>(count);
    }
    stream.close();
    if (!stream || total != expected_size) {
      return std::unexpected{"更新归档成员解压大小不一致。"};
    }
    if (auto valid = verify_asset_file(output, expected.members[*member_index].asset);
        !valid) {
      return valid;
    }
    seen[*member_index] = true;
  }
  if (!saw_entry || std::ranges::any_of(seen, [](bool value) { return !value; })) {
    return std::unexpected{"更新归档缺少签名 manifest 要求的成员。"};
  }
  return {};
}

}  // namespace awj::update
