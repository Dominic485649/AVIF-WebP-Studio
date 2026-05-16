module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>

export module avif.core;

import avif.config;

export namespace avif {

namespace fs = std::filesystem;

struct ImageFile {
  std::size_t index{};
  fs::path path{};
  fs::path relative_dir{};
  std::wstring source_extension_disambiguator{};
  std::uintmax_t bytes{};
  std::wstring date_token{};
  std::wstring time_token{};
  std::wstring datetime_token{};
  std::wstring unix_token{};
  std::wstring random_token{};
  std::wstring hash_token{};
  bool extension_disambiguated{};
};

struct EncodeResult {
  std::size_t index{};
  fs::path input_path{};
  fs::path output_path{};
  std::uintmax_t original_bytes{};
  std::uintmax_t output_bytes{};
  int quality{};
  int speed{};
  double seconds{};
  bool processed{false};
  bool ok{false};
  bool skipped{false};
  bool canceled{false};
  std::string message{};
  std::string command{};
};

void set_process_low_priority() noexcept {
  // 保留给需要整进程后台运行的调用方。UI 启动阶段不调用它，
  // 避免高负载时窗口初始化被普通优先级任务饿住。
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

void set_current_thread_low_priority() noexcept {
  // 只降低 CPU 调度优先级。THREAD_MODE_BACKGROUND_BEGIN 会连带降低 I/O
  // 和内存优先级，高负载时容易让图片读写表现成“卡住”。
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

namespace core_detail {

std::string narrow_ascii(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t ch : text) {
    out.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
  }
  return out;
}

bool has_path_separator(std::wstring_view text) {
  return text.find(L'\\') != std::wstring_view::npos ||
         text.find(L'/') != std::wstring_view::npos;
}

std::string trim_copy(std::string text) {
  const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  const auto first = std::ranges::find_if(text, not_space);
  const auto last = std::ranges::find_if(text | std::views::reverse, not_space)
                        .base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

std::string csv_escape(std::string value) {
  // summary.csv 可能被 Excel 打开；用户文件名不能被解释成公式执行。
  if (!value.empty() &&
      (value.front() == '=' || value.front() == '+' || value.front() == '-' ||
       value.front() == '@')) {
    value.insert(value.begin(), '\'');
  }
  if (value.find_first_of(",\"\r\n") == std::string::npos) {
    return value;
  }
  std::string out{"\""};
  for (const char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

void replace_all(std::wstring& text,
                 std::wstring_view token,
                 std::wstring_view value) {
  std::size_t pos = 0;
  while ((pos = text.find(token, pos)) != std::wstring::npos) {
    text.replace(pos, token.size(), value);
    pos += value.size();
  }
}

bool contains_token(std::wstring_view text, std::wstring_view token) {
  return text.find(token) != std::wstring_view::npos;
}

std::wstring sanitize_output_stem(std::wstring value, std::size_t index) {
  // 模板变量来自文件名和用户输入，必须清理 Windows 禁用字符和保留设备名。
  for (auto& ch : value) {
    const bool invalid = ch < L' ' || ch == L'<' || ch == L'>' || ch == L':' ||
                         ch == L'"' || ch == L'/' || ch == L'\\' ||
                         ch == L'|' || ch == L'?' || ch == L'*';
    if (invalid) {
      ch = L'_';
    }
  }

  while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) {
    value.pop_back();
  }
  if (value.empty()) {
    value = std::format(L"image-{:04}", index + 1);
  }

  auto reserved = value;
  const auto dot = reserved.find(L'.');
  if (dot != std::wstring::npos) {
    reserved.resize(dot);
  }
  std::ranges::transform(reserved, reserved.begin(),
                         [](wchar_t ch) { return std::towupper(ch); });
  const bool reserved_device =
      reserved == L"CON" || reserved == L"PRN" || reserved == L"AUX" ||
      reserved == L"NUL" ||
      (reserved.size() == 4 &&
       (reserved.starts_with(L"COM") || reserved.starts_with(L"LPT")) &&
       reserved[3] >= L'1' && reserved[3] <= L'9');
  if (reserved_device) {
    value.push_back(L'_');
  }
  return value;
}

}  // namespace core_detail

std::string utf8_from_wide(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                          nullptr);
  if (required <= 0) {
    return core_detail::narrow_ascii(text);
  }
  std::string out(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), required, nullptr, nullptr);
  return out;
}

std::wstring wide_from_utf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int required =
      MultiByteToWideChar(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    std::wstring fallback;
    fallback.reserve(text.size());
    for (const char ch : text) {
      fallback.push_back(static_cast<unsigned char>(ch));
    }
    return fallback;
  }
  std::wstring out(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), required);
  return out;
}

std::string path_to_utf8(const fs::path& path) {
  return utf8_from_wide(path.native());
}

std::wstring normalized_lower_path_key(const fs::path& path) {
  auto key = path.lexically_normal().wstring();
  std::ranges::transform(key, key.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  return key;
}

std::string win32_error_message(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(flags, nullptr, error, 0,
                                      reinterpret_cast<wchar_t*>(&buffer), 0,
                                      nullptr);
  if (length == 0 || buffer == nullptr) {
    return std::format("Win32 error {}", error);
  }
  std::wstring message{buffer, buffer + length};
  LocalFree(buffer);
  return core_detail::trim_copy(utf8_from_wide(message));
}

fs::path executable_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                    static_cast<DWORD>(buffer.size()));
  while (length == buffer.size()) {
    buffer.assign(buffer.size() * 2, L'\0');
    length = GetModuleFileNameW(nullptr, buffer.data(),
                                static_cast<DWORD>(buffer.size()));
  }
  buffer.resize(length);
  return fs::path{buffer}.parent_path();
}

class FileLogger {
 public:
  explicit FileLogger(fs::path output_dir, bool enabled = true)
      : enabled_{enabled},
        log_dir_{std::move(output_dir) / L"log"},
        log_file_{log_dir_ / L"avif-console.log"} {
    if (enabled_) {
      std::error_code ec;
      fs::create_directories(log_dir_, ec);
      if (ec) {
        enabled_ = false;
        last_error_ = std::format("无法创建日志目录 {}: {}",
                                  path_to_utf8(log_dir_), ec.message());
      } else {
        info("===== NEW SESSION START =====");
      }
    }
  }

  void info(std::string_view message) { append("INFO", message); }
  void warn(std::string_view message) { append("WARN", message); }
  void error(std::string_view message) { append("ERROR", message); }

  [[nodiscard]] bool enabled() const {
    std::scoped_lock lock{mutex_};
    return enabled_;
  }

  [[nodiscard]] std::string last_error() const {
    std::scoped_lock lock{mutex_};
    return last_error_;
  }

 private:
  void append(std::string_view level, std::string_view message) {
    std::scoped_lock lock{mutex_};
    if (!enabled_) {
      return;
    }
    std::ofstream stream{log_file_, std::ios::app | std::ios::binary};
    if (!stream) {
      enabled_ = false;
      last_error_ = std::format("无法写入日志文件 {}", path_to_utf8(log_file_));
      return;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    stream << std::format("[{:%F %T}] [{}] {}\n", now, level, message);
  }

  bool enabled_{true};
  fs::path log_dir_;
  fs::path log_file_;
  std::string last_error_{};
  mutable std::mutex mutex_;
};

bool is_supported_image_extension(const fs::path& path) {
  auto ext = path.extension().wstring();
  std::ranges::transform(ext, ext.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  return ext == L".jpg" || ext == L".jpeg" || ext == L".png" ||
         ext == L".webp" || ext == L".bmp" || ext == L".tif" ||
         ext == L".tiff" || ext == L".gif" || ext == L".jxl" ||
         ext == L".jp2" || ext == L".heic" || ext == L".heif" ||
         ext == L".avif";
}

fs::path default_output_dir_for(const fs::path& input_path) {
  std::error_code ec;
  if (fs::is_regular_file(input_path, ec) && !ec) {
    const auto parent = input_path.parent_path();
    return parent.empty() ? fs::current_path() : parent;
  }
  return input_path;
}

fs::path output_dir_for(const AppConfig& cfg) {
  if (!cfg.output_dir.empty()) {
    return cfg.output_dir;
  }
  return default_output_dir_for(cfg.input_path);
}

std::wstring output_extension_for(OutputFormat format) {
  switch (format) {
    case OutputFormat::webp:
      return L".webp";
    case OutputFormat::avif:
    default:
      return L".avif";
  }
}

std::string output_format_name(OutputFormat format) {
  switch (format) {
    case OutputFormat::webp:
      return "WEBP";
    case OutputFormat::avif:
    default:
      return "AVIF";
  }
}

ImageFile make_image_file(std::size_t index,
                          const fs::path& path,
                          fs::path relative_dir,
                          std::uintmax_t bytes,
                          std::mt19937_64& rng,
                          std::wstring hash_token = {}) {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto unix_seconds = seconds.time_since_epoch().count();
  const auto random_value = rng();
  return ImageFile{.index = index,
                   .path = path,
                   .relative_dir = std::move(relative_dir),
                   .bytes = bytes,
                   .date_token = std::format(L"{:%Y%m%d}", seconds),
                   .time_token = std::format(L"{:%H%M%S}", seconds),
                   .datetime_token = std::format(L"{:%Y%m%d-%H%M%S}", seconds),
                   .unix_token = std::format(L"{}", unix_seconds),
                   .random_token = std::format(L"{:08x}",
                                               static_cast<unsigned int>(
                                                   random_value & 0xffffffffu)),
                   .hash_token = std::move(hash_token)};
}

std::expected<void, std::string> file_hash_token(const fs::path& path,
                                                  std::wstring& out) {
  constexpr std::uint64_t offset = 14695981039346656037ull;
  constexpr std::uint64_t prime = 1099511628211ull;
  std::uint64_t hash = offset;

  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::unexpected{
        std::format("无法读取用于 {{hash}}/{{hash8}} 的文件内容: {}。请检查文件是否仍存在、是否被占用，或当前用户是否有读取权限。",
                    path_to_utf8(path))};
  }

  std::array<char, 64 * 1024> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = stream.gcount();
    for (std::streamsize i = 0; i < read; ++i) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
      hash *= prime;
    }
  }
  if (!stream.eof()) {
    return std::unexpected{
        std::format("读取文件哈希时发生 I/O 错误: {}。请检查磁盘、权限或杀毒软件拦截。",
                    path_to_utf8(path))};
  }
  out = std::format(L"{:016x}", hash);
  return {};
}

fs::path relative_output_dir(const fs::path& input_root,
                             const fs::path& image_path) {
  // 文件夹输入时记录图片相对输入根目录的位置，输出时据此重建原目录结构。
  std::error_code ec;
  const auto root = fs::absolute(input_root, ec);
  if (ec) {
    return {};
  }
  const auto parent = fs::absolute(image_path.parent_path(), ec);
  if (ec) {
    return {};
  }
  auto relative = fs::relative(parent, root, ec);
  if (ec || relative.empty() || relative == L".") {
    return {};
  }
  return relative;
}

std::wstring output_name_for(const AppConfig& cfg, const ImageFile& image);

std::wstring source_extension_disambiguator(const fs::path& path) {
  auto ext = path.extension().wstring();
  if (ext.empty()) {
    return L".source";
  }
  if (ext.front() != L'.') {
    ext.insert(ext.begin(), L'.');
  }
  return ext;
}

fs::path planned_output_path_for(const AppConfig& cfg, const ImageFile& image) {
  auto output_dir = output_dir_for(cfg);
  if (!image.relative_dir.empty()) {
    output_dir /= image.relative_dir;
  }
  return output_dir / output_name_for(cfg, image);
}

void apply_source_extension_disambiguation(const AppConfig& cfg,
                                           std::vector<ImageFile>& files) {
  // 例如 1.jpg 和 1.bmp 都套用 {name}.avif 时会同名；保留源扩展避免互相覆盖。
  std::unordered_map<std::wstring, std::vector<std::size_t>> by_output;
  for (std::size_t i = 0; i < files.size(); ++i) {
    by_output[normalized_lower_path_key(planned_output_path_for(cfg, files[i]))]
        .push_back(i);
  }

  for (const auto& [_, indices] : by_output) {
    if (indices.size() < 2) {
      continue;
    }

    std::unordered_map<std::wstring, int> source_extensions;
    for (const auto index : indices) {
      auto ext = files[index].path.extension().wstring();
      std::ranges::transform(ext, ext.begin(),
                             [](wchar_t ch) { return std::towlower(ch); });
      source_extensions.try_emplace(std::move(ext), 0);
    }
    if (source_extensions.size() < 2) {
      continue;
    }

    for (const auto index : indices) {
      files[index].source_extension_disambiguator =
          source_extension_disambiguator(files[index].path);
      files[index].extension_disambiguated = true;
    }
  }
}

std::expected<void, std::string> scan_images(const AppConfig& cfg,
                                             std::vector<ImageFile>& files) {
  // 输入可以是单张图片或一个目录。这里不再抛异常，而是把失败原因、路径和建议
  // 作为 std::expected 的 error 返回给 CLI/UI，避免用户只看到“未知异常”。
  std::error_code ec;
  const auto input_path = cfg.input_path;
  const bool exists = fs::exists(input_path, ec);
  if (ec) {
    return std::unexpected{
        std::format("检查输入路径失败: {}；系统错误：{}。请确认路径可访问，或尝试用管理员权限/本地磁盘路径。",
                    path_to_utf8(input_path), ec.message())};
  }
  if (!exists) {
    return std::unexpected{
        std::format("输入路径不存在: {}。请检查路径是否写错、盘符是否挂载，或文件是否已被移动。",
                    path_to_utf8(input_path))};
  }

  std::random_device random_device;
  std::mt19937_64 rng{random_device()};

  files.clear();
  const bool template_needs_hash =
      core_detail::contains_token(cfg.output_template, L"{hash}") ||
      core_detail::contains_token(cfg.output_template, L"{hash8}");
  const auto build_hash = [&](const fs::path& path,
                              std::wstring& out) -> std::expected<void, std::string> {
    out.clear();
    if (!template_needs_hash) {
      return {};
    }
    return file_hash_token(path, out);
  };

  if (fs::is_regular_file(input_path, ec) && !ec) {
    if (!is_supported_image_extension(input_path)) {
      return std::unexpected{
          std::format("输入文件格式不受支持: {}。支持 jpg/jpeg/png/webp/bmp/tif/tiff/gif/jxl/jp2/heic/heif/avif。",
                      path_to_utf8(input_path))};
    }
    auto bytes = fs::file_size(input_path, ec);
    if (ec) {
      bytes = 0;
      ec.clear();
    }
    std::wstring hash;
    if (auto ok = build_hash(input_path, hash); !ok) {
      return std::unexpected{ok.error()};
    }
    files.push_back(make_image_file(0, input_path, {}, bytes, rng, std::move(hash)));
    apply_source_extension_disambiguation(cfg, files);
    return {};
  }

  if (ec) {
    return std::unexpected{
        std::format("判断输入路径类型失败: {}；系统错误：{}。",
                    path_to_utf8(input_path), ec.message())};
  }
  if (!fs::is_directory(input_path, ec) || ec) {
    return std::unexpected{
        std::format("输入路径不是文件或文件夹: {}。请确认输入模式和实际路径一致。",
                    path_to_utf8(input_path))};
  }

  std::size_t skipped_access = 0;
  for (fs::recursive_directory_iterator it{
           input_path, fs::directory_options::skip_permission_denied, ec},
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ++skipped_access;
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec) || ec) {
      if (ec) {
        ++skipped_access;
      }
      ec.clear();
      continue;
    }
    if (!is_supported_image_extension(it->path())) {
      continue;
    }

    auto bytes = fs::file_size(it->path(), ec);
    if (ec) {
      bytes = 0;
      ec.clear();
    }
    std::wstring hash;
    if (auto ok = build_hash(it->path(), hash); !ok) {
      return std::unexpected{ok.error()};
    }
    files.push_back(make_image_file(
        files.size(), it->path(), relative_output_dir(input_path, it->path()),
        bytes, rng, std::move(hash)));
    ec.clear();
  }

  if (files.empty() && skipped_access > 0) {
    return std::unexpected{
        std::format("未找到可转换图片，并且扫描时跳过了 {} 个无权限或不可访问的条目。请检查目录权限或复制到本地目录后重试。",
                    skipped_access)};
  }

  std::ranges::sort(files, [](const ImageFile& left, const ImageFile& right) {
    return left.path.native() < right.path.native();
  });
  for (std::size_t i = 0; i < files.size(); ++i) {
    files[i].index = i;
  }
  apply_source_extension_disambiguation(cfg, files);
  return {};
}

std::wstring encode_params_token_for(const AppConfig& cfg) {
  std::wstring token = std::format(L"q{}", cfg.quality);
  if (cfg.magick_speed) {
    token += std::format(L"t{}", *cfg.magick_speed);
  }
  if (cfg.chroma_mode != ChromaMode::auto_keep) {
    switch (cfg.chroma_mode) {
      case ChromaMode::yuv444:
        token += L"_444";
        break;
      case ChromaMode::yuv422:
        token += L"_422";
        break;
      case ChromaMode::yuv420:
        token += L"_420";
        break;
      case ChromaMode::auto_keep:
        break;
    }
  }
  if (cfg.bit_depth) {
    token += std::format(L"_{}", *cfg.bit_depth);
  }
  return token;
}

std::wstring output_name_for(const AppConfig& cfg, const ImageFile& image) {
  std::wstring name = cfg.output_template;
  auto stem = image.path.stem().wstring();
  auto ext = image.path.extension().wstring();
  if (!ext.empty() && ext.front() == L'.') {
    ext.erase(ext.begin());
  }

  core_detail::replace_all(name, L"{index}", std::format(L"{:04}", image.index + 1));
  core_detail::replace_all(name, L"{name}", stem);
  core_detail::replace_all(name, L"{ext}", ext);
  core_detail::replace_all(name, L"{date}", image.date_token);
  core_detail::replace_all(name, L"{time}", image.time_token);
  core_detail::replace_all(name, L"{datetime}", image.datetime_token);
  core_detail::replace_all(name, L"{unix}", image.unix_token);
  core_detail::replace_all(name, L"{rand}", image.random_token);
  core_detail::replace_all(name, L"{params}", encode_params_token_for(cfg));

  if (core_detail::contains_token(name, L"{hash}")) {
    const auto hash = image.hash_token.empty()
                          ? std::wstring{L"hash-unavailable"}
                          : image.hash_token;
    core_detail::replace_all(name, L"{hash}", hash);
  }
  if (core_detail::contains_token(name, L"{hash8}")) {
    const auto hash = image.hash_token.empty()
                          ? std::wstring{L"hash-unavailable"}
                          : image.hash_token;
    core_detail::replace_all(name, L"{hash8}", hash.substr(0, 8));
  }

  name = core_detail::sanitize_output_stem(std::move(name), image.index);
  name += image.source_extension_disambiguator;
  name += output_extension_for(cfg.output_format);
  return name;
}

fs::path output_path_for(const AppConfig& cfg, const ImageFile& image) {
  // 同格式转换且输出路径等于输入路径时，自动改名，避免默认覆盖源文件。
  auto output = planned_output_path_for(cfg, image);
  std::error_code output_ec;
  std::error_code image_ec;
  const auto output_key = normalized_lower_path_key(fs::absolute(output, output_ec));
  const auto image_key = normalized_lower_path_key(fs::absolute(image.path, image_ec));
  if (output_ec || image_ec || output_key != image_key) {
    return output;
  }

  const auto extension = output.extension().wstring();
  const auto stem = output.stem().wstring();
  output.replace_filename(stem + L"-converted" + extension);
  return output;
}

std::string format_size(std::uintmax_t bytes) {
  constexpr double kib = 1024.0;
  constexpr double mib = kib * 1024.0;
  if (bytes >= static_cast<std::uintmax_t>(mib)) {
    return std::format("{:.2f} MiB", static_cast<double>(bytes) / mib);
  }
  if (bytes >= static_cast<std::uintmax_t>(kib)) {
    return std::format("{:.1f} KiB", static_cast<double>(bytes) / kib);
  }
  return std::format("{} B", bytes);
}

std::expected<void, std::string> write_csv(const fs::path& output_dir,
                                           std::span<const EncodeResult> results) {
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    return std::unexpected{std::format(
        "无法创建报告目录: {}；系统错误：{}。请确认输出目录可写、磁盘未满，且路径没有被其他程序锁定。",
        path_to_utf8(output_dir), ec.message())};
  }

  std::ofstream csv{output_dir / L"summary.csv", std::ios::binary};
  if (!csv) {
    return std::unexpected{std::format(
        "无法写入报告文件: {}。请检查文件是否正被 Excel/表格软件打开，或输出目录是否可写。",
        path_to_utf8(output_dir / L"summary.csv"))};
  }

  csv << "\xEF\xBB\xBF";
  csv << "index,input,output,original_bytes,output_bytes,ratio,quality,speed,"
         "seconds,status,message,command\n";

  for (const auto& result : results) {
    const double ratio =
        result.original_bytes == 0
            ? 0.0
            : static_cast<double>(result.output_bytes) /
                  static_cast<double>(result.original_bytes);
    const char* status = result.ok
                             ? (result.skipped ? "skipped" : "ok")
                             : (result.canceled
                                    ? "canceled"
                                    : (result.processed ? "failed" : "pending"));
    const std::string speed =
        result.speed < 0 ? "default" : std::to_string(result.speed);
    csv << (result.index + 1) << ','
        << core_detail::csv_escape(path_to_utf8(result.input_path)) << ','
        << core_detail::csv_escape(path_to_utf8(result.output_path)) << ','
        << result.original_bytes << ',' << result.output_bytes << ','
        << std::format("{:.4f}", ratio) << ',' << result.quality << ','
        << speed << ',' << std::format("{:.3f}", result.seconds) << ','
        << status << ',' << core_detail::csv_escape(result.message) << ','
        << core_detail::csv_escape(result.command) << '\n';
  }

  if (!csv) {
    return std::unexpected{
        std::format("写入报告文件失败: {}", path_to_utf8(output_dir / L"summary.csv"))};
  }
  return {};
}

}  // namespace avif
