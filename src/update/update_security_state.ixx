module;

#include <sodium.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module awj.update_security_state;

export namespace awj::update {

inline constexpr std::string_view update_security_state_file_name =
    ".awj-update-security-state.json";

enum class UpdateSecurityDocument {
  legacy_manifest,
  archive_manifest_v2,
  keyring,
};

namespace security_state_detail {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

constexpr std::size_t maximum_state_bytes = 64 * 1024;
constexpr std::string_view state_lock_file_name = ".awj-update-security-state.lock";

struct Record {
  std::uint64_t sequence{};
  std::string sha256{};
};

struct State {
  Record legacy{};
  Record archive_v2{};
  Record keyring{};
};

std::string_view record_name(UpdateSecurityDocument document) noexcept {
  switch (document) {
    case UpdateSecurityDocument::legacy_manifest:
      return "v1";
    case UpdateSecurityDocument::archive_manifest_v2:
      return "v2";
    case UpdateSecurityDocument::keyring:
    default:
      return "keyring";
  }
}

Record& select_record(State& state, UpdateSecurityDocument document) noexcept {
  switch (document) {
    case UpdateSecurityDocument::legacy_manifest:
      return state.legacy;
    case UpdateSecurityDocument::archive_manifest_v2:
      return state.archive_v2;
    case UpdateSecurityDocument::keyring:
    default:
      return state.keyring;
  }
}

bool valid_sha256(std::string_view value) noexcept {
  return value.size() == crypto_hash_sha256_BYTES * 2 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::expected<fs::path, std::string> current_executable_directory() {
#ifdef _WIN32
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::unexpected{"无法定位更新安全状态的可执行文件目录。"};
    }
    if (length < buffer.size()) {
      return fs::path{std::wstring_view{buffer.data(), length}}.parent_path();
    }
    if (buffer.size() > 1024u * 1024u) {
      return std::unexpected{"更新安全状态的可执行文件路径过长。"};
    }
    buffer.resize(buffer.size() * 2, L'\0');
  }
#else
  std::vector<char> buffer(4096, '\0');
  while (true) {
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return std::unexpected{"无法定位更新安全状态的可执行文件目录。"};
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return fs::path{std::string_view{buffer.data(),
                                       static_cast<std::size_t>(length)}}
          .parent_path();
    }
    if (buffer.size() > 1024u * 1024u) {
      return std::unexpected{"更新安全状态的可执行文件路径过长。"};
    }
    buffer.resize(buffer.size() * 2, '\0');
  }
#endif
}

class StateLock {
 public:
  StateLock() = default;
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
  StateLock(StateLock&& other) noexcept { *this = std::move(other); }
  StateLock& operator=(StateLock&& other) noexcept {
    if (this == &other) return *this;
    close();
#ifdef _WIN32
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
    return *this;
  }
  ~StateLock() { close(); }

  static std::expected<StateLock, std::string> acquire(const fs::path& directory) {
    StateLock result;
    const auto path = directory / state_lock_file_name;
#ifdef _WIN32
    result.handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE |
                                     FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                 nullptr);
    if (result.handle_ == INVALID_HANDLE_VALUE) {
      return std::unexpected{"无法打开更新安全状态锁文件。"};
    }
    OVERLAPPED overlapped{};
    if (LockFileEx(result.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0,
                   &overlapped) == FALSE) {
      return std::unexpected{"无法锁定更新安全状态文件。"};
    }
#else
    result.fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                        0600);
    if (result.fd_ < 0) {
      return std::unexpected{"无法打开更新安全状态锁文件。"};
    }
    while (::flock(result.fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        return std::unexpected{"无法锁定更新安全状态文件。"};
      }
    }
#endif
    return result;
  }

 private:
  void close() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, 1, 0, &overlapped);
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int fd_{-1};
#endif
};

std::expected<Record, std::string> parse_record(const Json& document,
                                                 std::string_view name) {
  const auto key = std::string{name};
  if (!document.contains(key) || !document.at(key).is_object()) {
    return std::unexpected{std::format("更新安全状态缺少 {} 记录。", name)};
  }
  const auto& source = document.at(key);
  if (!source.contains("sequence") || !source.at("sequence").is_number_unsigned() ||
      !source.contains("sha256") || !source.at("sha256").is_string()) {
    return std::unexpected{std::format("更新安全状态 {} 记录类型错误。", name)};
  }
  auto record = Record{.sequence = source.at("sequence").get<std::uint64_t>(),
                       .sha256 = source.at("sha256").get<std::string>()};
  if ((record.sequence == 0 && !record.sha256.empty()) ||
      (record.sequence != 0 && !valid_sha256(record.sha256))) {
    return std::unexpected{std::format("更新安全状态 {} 记录哈希非法。", name)};
  }
  return record;
}

std::expected<State, std::string> parse_state(std::string_view raw) {
  try {
    const auto document = Json::parse(raw.begin(), raw.end());
    if (!document.is_object() || !document.contains("schema") ||
        !document.at("schema").is_number_unsigned() ||
        document.at("schema").get<std::uint32_t>() != 1) {
      return std::unexpected{"更新安全状态 schema 非法。"};
    }
    auto legacy = parse_record(document, "v1");
    auto archive = parse_record(document, "v2");
    auto keyring = parse_record(document, "keyring");
    if (!legacy || !archive || !keyring) {
      return std::unexpected{!legacy ? legacy.error()
                          : !archive ? archive.error()
                                     : keyring.error()};
    }
    return State{.legacy = std::move(*legacy),
                 .archive_v2 = std::move(*archive),
                 .keyring = std::move(*keyring)};
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{
        std::format("更新安全状态损坏，已拒绝更新：{}", error.what())};
  }
}

std::string serialize_state(const State& state) {
  const auto record = [](const Record& value) {
    return Json{{"sequence", value.sequence}, {"sha256", value.sha256}};
  };
  const Json document{{"schema", 1},
                      {"v1", record(state.legacy)},
                      {"v2", record(state.archive_v2)},
                      {"keyring", record(state.keyring)}};
  return document.dump() + "\n";
}

std::expected<State, std::string> read_state_or_default(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) return State{};
    return std::unexpected{"无法检查更新安全状态文件。"};
  }
  if (!fs::exists(status)) return State{};
  if (!fs::is_regular_file(status)) {
    return std::unexpected{"更新安全状态文件不是常规文件，已拒绝更新。"};
  }
  const auto size = fs::file_size(path, ec);
  if (ec || size == 0 || size > maximum_state_bytes ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected{"更新安全状态文件大小非法，已拒绝更新。"};
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::unexpected{"无法读取更新安全状态文件，已拒绝更新。"};
  std::string raw(static_cast<std::size_t>(size), '\0');
  input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
  if (!input && !input.eof()) {
    return std::unexpected{"读取更新安全状态文件失败，已拒绝更新。"};
  }
  return parse_state(raw);
}

std::expected<std::string, std::string> sha256(std::string_view raw) {
  if (sodium_init() < 0) return std::unexpected{"初始化更新状态 SHA-256 失败。"};
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(),
                     reinterpret_cast<const unsigned char*>(raw.data()),
                     static_cast<unsigned long long>(raw.size()));
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  return std::string{hex.data()};
}

#ifdef _WIN32
std::expected<void, std::string> write_state_atomically(const fs::path& path,
                                                         std::string_view text) {
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    auto temporary = path;
    temporary += std::format(L".tmp-{}-{}", GetCurrentProcessId(), attempt);
    const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_EXISTS) continue;
      return std::unexpected{"无法创建更新安全状态临时文件。"};
    }
    bool written = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto remaining = std::min<std::size_t>(
          text.size() - offset, std::numeric_limits<DWORD>::max());
      DWORD count = 0;
      if (WriteFile(handle, text.data() + offset, static_cast<DWORD>(remaining),
                    &count, nullptr) == FALSE || count == 0) {
        written = false;
        break;
      }
      offset += count;
    }
    const bool flushed = written && FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!flushed) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return std::unexpected{"无法刷新更新安全状态临时文件。"};
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return std::unexpected{"无法原子替换更新安全状态文件。"};
    }
    return {};
  }
  return std::unexpected{"无法分配更新安全状态临时文件。"};
}
#else
std::expected<void, std::string> fsync_directory(const fs::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return std::unexpected{"无法刷新更新安全状态目录。"};
  const bool flushed = ::fsync(fd) == 0;
  ::close(fd);
  if (!flushed) return std::unexpected{"无法刷新更新安全状态目录。"};
  return {};
}

std::expected<void, std::string> write_state_atomically(const fs::path& path,
                                                         std::string_view text) {
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto temporary = path.string() +
                           std::format(".tmp-{}-{}", ::getpid(), attempt);
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return std::unexpected{"无法创建更新安全状态临时文件。"};
    }
    bool written = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto count = ::write(fd, text.data() + offset, text.size() - offset);
      if (count < 0) {
        if (errno == EINTR) continue;
        written = false;
        break;
      }
      if (count == 0) {
        written = false;
        break;
      }
      offset += static_cast<std::size_t>(count);
    }
    const bool flushed = written && ::fsync(fd) == 0;
    ::close(fd);
    if (!flushed) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return std::unexpected{"无法刷新更新安全状态临时文件。"};
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
      std::error_code ec;
      fs::remove(temporary, ec);
      return std::unexpected{"无法原子替换更新安全状态文件。"};
    }
    return fsync_directory(path.parent_path());
  }
  return std::unexpected{"无法分配更新安全状态临时文件。"};
}
#endif

}  // namespace security_state_detail

std::expected<void, std::string> accept_verified_update_document(
    UpdateSecurityDocument document, std::uint64_t sequence,
    std::string_view raw_bytes, std::uint64_t legacy_sequence_floor = 0,
    std::filesystem::path state_directory = {}) {
  using namespace security_state_detail;
  if (sequence == 0 || raw_bytes.empty()) {
    return std::unexpected{"更新安全状态拒绝空文档或零 sequence。"};
  }
  if (state_directory.empty()) {
    auto executable_directory = current_executable_directory();
    if (!executable_directory) return std::unexpected{executable_directory.error()};
    state_directory = std::move(*executable_directory);
  }
  std::error_code ec;
  if (!fs::is_directory(state_directory, ec) || ec) {
    return std::unexpected{"更新安全状态目录不可用，已拒绝更新。"};
  }
  auto lock = StateLock::acquire(state_directory);
  if (!lock) return std::unexpected{lock.error()};
  const auto state_path = state_directory / update_security_state_file_name;
  auto state = read_state_or_default(state_path);
  if (!state) return std::unexpected{state.error()};
  auto digest = sha256(raw_bytes);
  if (!digest) return std::unexpected{digest.error()};
  auto& record = select_record(*state, document);
  const auto floor = std::max(record.sequence, legacy_sequence_floor);
  if (sequence < floor) {
    return std::unexpected{std::format(
        "更新安全状态拒绝 {} sequence 回退（收到 {}，本机至少为 {}）。",
        record_name(document), sequence, floor)};
  }
  if (sequence == record.sequence && !record.sha256.empty() &&
      record.sha256 != *digest) {
    return std::unexpected{std::format(
        "更新安全状态拒绝 {} 在相同 sequence 下变更内容。", record_name(document))};
  }
  if (sequence == record.sequence && record.sha256 == *digest) {
    return {};
  }
  record.sequence = sequence;
  record.sha256 = std::move(*digest);
  if (auto saved = write_state_atomically(state_path, serialize_state(*state)); !saved) {
    return std::unexpected{saved.error()};
  }
  return {};
}

}  // namespace awj::update
