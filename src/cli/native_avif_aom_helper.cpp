#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <print>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

#include <scn/scan.h>

import awj.avif_aom_codec;
import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.raw_image_io;
import awj.resource_planner;
import awj.webp_codec;

namespace {

enum class HelperMode { encode, decode };

void print_line(std::string_view text) {
  std::println("{}", text);
}

std::expected<int, std::string> parse_int(std::wstring_view value,
                                          std::string_view name,
                                          int minimum,
                                          int maximum) {
  const auto text = awj::utf8_from_wide(value);
  const std::string_view source{text};
  if (!source.empty() &&
      std::isspace(static_cast<unsigned char>(source.front())) != 0) {
    return std::unexpected{std::format("{} 参数无效。", name)};
  }
  const auto parsed = scn::scan_int<int>(source);
  if (!parsed || parsed->begin() != parsed->end() ||
      parsed->value() < minimum || parsed->value() > maximum) {
    return std::unexpected{std::format("{} 参数无效。", name)};
  }
  return parsed->value();
}

std::string lower_ascii(std::wstring_view value) {
  auto text = awj::utf8_from_wide(value);
  std::ranges::transform(text, text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

std::expected<HelperMode, std::string> parse_mode(std::wstring_view value) {
  const auto text = lower_ascii(value);
  if (text == "encode") {
    return HelperMode::encode;
  }
  if (text == "decode") {
    return HelperMode::decode;
  }
  return std::unexpected{"mode 参数无效。"};
}

std::expected<awj::AvifEncoderMode, std::string> parse_encoder(std::wstring_view value) {
  const auto text = lower_ascii(value);
  if (text == "auto") {
    return awj::AvifEncoderMode::automatic;
  }
  if (text == "aom") {
    return awj::AvifEncoderMode::aom;
  }
  if (text == "svt" || text == "svt-av1" || text == "svt-av1-hdr") {
    return std::unexpected{"svt-av1-hdr 由官方静态 SvtAv1EncApp helper 处理，不通过 AWJ-native-avif-helper。"};
  }
  if (text == "zenrav1e") {
    return awj::AvifEncoderMode::zenrav1e;
  }
  if (text == "rav1e") {
    return std::unexpected{"rav1e 不可用；zenrav1e 必须通过 Rust zenravif 静态库实现。"};
  }
  return std::unexpected{"encoder 参数无效。"};
}

std::expected<awj::ChromaMode, std::string> parse_chroma(std::wstring_view value) {
  const auto text = lower_ascii(value);
  if (text == "420" || text == "yuv420") {
    return awj::ChromaMode::yuv420;
  }
  if (text == "444" || text == "yuv444") {
    return awj::ChromaMode::yuv444;
  }
  if (text == "422" || text == "yuv422") {
    return awj::ChromaMode::yuv422;
  }
  if (text == "auto") {
    return awj::ChromaMode::auto_keep;
  }
  return std::unexpected{"chroma 参数无效。"};
}

struct HelperConfig {
  HelperMode mode{HelperMode::encode};
  awj::AvifEncoderMode encoder{awj::AvifEncoderMode::aom};
  std::filesystem::path input{};
  std::filesystem::path output{};
  int quality{90};
  int speed{6};
  int bit_depth{8};
  awj::ChromaMode chroma{awj::ChromaMode::yuv420};
  int threads{1};
  std::optional<awj::HdrContentLightMetadata> source_content_light{};
  bool experimental_encoders{};
};

std::expected<HelperConfig, std::string> parse_args(int argc, wchar_t* argv[]) try {
  HelperConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg{argv[i]};
    const auto require_value = [&](std::string_view name) -> std::expected<std::wstring_view, std::string> {
      if (i + 1 >= argc) {
        return std::unexpected{std::format("{} 缺少参数值。", name)};
      }
      return std::wstring_view{argv[++i]};
    };
    if (arg == L"--mode") {
      auto value = require_value("--mode");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_mode(*value);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.mode = *parsed;
      continue;
    }
    if (arg == L"--encoder") {
      auto value = require_value("--encoder");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_encoder(*value);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.encoder = *parsed;
      continue;
    }
    if (arg == L"--input") {
      auto value = require_value("--input");
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.input = std::filesystem::path{std::wstring{*value}};
      continue;
    }
    if (arg == L"--output") {
      auto value = require_value("--output");
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.output = std::filesystem::path{std::wstring{*value}};
      continue;
    }
    if (arg == L"--quality") {
      auto value = require_value("--quality");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_int(*value, "quality", 1, 100);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.quality = *parsed;
      continue;
    }
    if (arg == L"--speed") {
      auto value = require_value("--speed");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_int(*value, "speed", 0, 10);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.speed = *parsed;
      continue;
    }
    if (arg == L"--bit-depth") {
      auto value = require_value("--bit-depth");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_int(*value, "bit-depth", 1, 16);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.bit_depth = *parsed;
      continue;
    }
    if (arg == L"--chroma") {
      auto value = require_value("--chroma");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_chroma(*value);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.chroma = *parsed;
      continue;
    }
    if (arg == L"--threads") {
      auto value = require_value("--threads");
      if (!value) {
        return std::unexpected{value.error()};
      }
      auto parsed = parse_int(*value, "threads", 1, 256);
      if (!parsed) {
        return std::unexpected{parsed.error()};
      }
      cfg.threads = *parsed;
      continue;
    }
    if (arg == L"--source-content-light") {
      auto max_cll = require_value("--source-content-light");
      if (!max_cll) {
        return std::unexpected{max_cll.error()};
      }
      auto max_pall = require_value("--source-content-light");
      if (!max_pall) {
        return std::unexpected{max_pall.error()};
      }
      auto parsed_max_cll = parse_int(*max_cll, "source-content-light maxCLL", 0, 65535);
      if (!parsed_max_cll) {
        return std::unexpected{parsed_max_cll.error()};
      }
      auto parsed_max_pall = parse_int(*max_pall, "source-content-light maxPALL", 0, 65535);
      if (!parsed_max_pall) {
        return std::unexpected{parsed_max_pall.error()};
      }
      cfg.source_content_light = awj::HdrContentLightMetadata{
          .max_cll = static_cast<std::uint16_t>(*parsed_max_cll),
          .max_pall = static_cast<std::uint16_t>(*parsed_max_pall)};
      continue;
    }
    if (arg == L"--experimental-encoders") {
      cfg.experimental_encoders = true;
      continue;
    }
    return std::unexpected{std::format("未知参数: {}", awj::utf8_from_wide(arg))};
  }
  if (cfg.input.empty() || cfg.output.empty()) {
    return std::unexpected{"--input 和 --output 不能为空。"};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"native AVIF helper 参数解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"native AVIF helper 参数解析数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"native AVIF helper 参数解析文件系统访问失败。"};
}

std::expected<awj::ImageDecodeResult, std::string> decode_input(
    const std::filesystem::path& path,
    int decode_threads) {
  try {
    const auto clamped_decode_threads = std::max(1, decode_threads);
    awj::WebPImageDecoder webp;
    if (webp.can_decode(path)) {
      return webp.decode(path);
    }
    awj::JXLImageDecoder jxl{clamped_decode_threads};
    if (jxl.can_decode(path)) {
      return jxl.decode(path);
    }
    awj::AvifImageDecoder avif{clamped_decode_threads};
    if (avif.can_decode(path)) {
      return avif.decode(path);
    }
    return std::unexpected{std::format("native AVIF helper 不支持输入格式: {}",
                                       awj::path_to_utf8(path.extension()))};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"native AVIF helper 解码输入内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"native AVIF helper 解码输入数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"native AVIF helper 解码输入文件系统访问失败。"};
  }
}

struct FileHandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueFileHandle = std::unique_ptr<void, FileHandleDeleter>;

class OutputFileCleanup {
 public:
  explicit OutputFileCleanup(const std::filesystem::path& path) noexcept : path_{&path} {}
  ~OutputFileCleanup() { cleanup(); }
  OutputFileCleanup(const OutputFileCleanup&) = delete;
  OutputFileCleanup& operator=(const OutputFileCleanup&) = delete;

  void release() noexcept { active_ = false; }

 private:
  void cleanup() noexcept {
    if (!active_) {
      return;
    }
    try {
      std::error_code ec;
      std::filesystem::remove(*path_, ec);
    } catch (...) {
    }
  }

  const std::filesystem::path* path_{};
  bool active_{true};
};

std::expected<void, std::string> write_bytes(const std::filesystem::path& path,
                                             const std::vector<std::byte>& bytes) {
  try {
    if (bytes.empty()) {
      return std::unexpected{std::format("输出内容为空，无法写入输出文件: {}", awj::display_path_for_user(path))};
    }
    if (bytes.size() > awj::encoding_defaults::max_input_file_bytes) {
      return std::unexpected{std::format("输出内容超过 20 GiB 运行时上限，无法写入输出文件: {}",
                                         awj::display_path_for_user(path))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建输出目录: {}；系统错误：{}",
                                           awj::display_path_for_user(parent), ec.message())};
      }
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        return std::unexpected{std::format("输出文件已存在: {}", awj::display_path_for_user(path))};
      }
      return std::unexpected{std::format("无法创建输出文件: {}；系统错误：{}",
                                         awj::display_path_for_user(path),
                                         awj::win32_error_message(error))};
    }
    OutputFileCleanup cleanup{path};
    UniqueFileHandle handle{file};

    auto remaining = bytes.size();
    const auto* cursor = reinterpret_cast<const std::uint8_t*>(bytes.data());
    while (remaining > 0) {
      const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
      DWORD written = 0;
      if (!WriteFile(handle.get(), cursor, chunk, &written, nullptr)) {
        return std::unexpected{std::format("写入输出文件失败: {}；系统错误：{}",
                                           awj::display_path_for_user(path),
                                           awj::win32_error_message(GetLastError()))};
      }
      if (written == 0) {
        return std::unexpected{std::format("写入输出文件失败: {}", awj::display_path_for_user(path))};
      }
      cursor += written;
      remaining -= written;
    }
    if (!FlushFileBuffers(handle.get())) {
      return std::unexpected{std::format("刷新输出文件失败: {}；系统错误：{}",
                                         awj::display_path_for_user(path),
                                         awj::win32_error_message(GetLastError()))};
    }
    const HANDLE output_handle = handle.get();
    if (!CloseHandle(output_handle)) {
      return std::unexpected{std::format("关闭输出文件失败: {}；系统错误：{}",
                                         awj::display_path_for_user(path),
                                         awj::win32_error_message(GetLastError()))};
    }
    handle.release();
    cleanup.release();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"输出文件写入内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"输出文件写入数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"输出文件写入时文件系统访问失败。"};
  }
}

std::expected<void, std::string> run_encode(const HelperConfig& cfg) {
  try {
    auto image = awj::read_raw_image_file(cfg.input);
    if (!image) {
      return std::unexpected{image.error()};
    }
    if (image->width == 0 || image->height == 0 ||
        image->width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        image->height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      return std::unexpected{"native AVIF helper 输入尺寸超过 encoder selection 限制。"};
    }
    if (image->width > std::numeric_limits<std::uint64_t>::max() / image->height) {
      return std::unexpected{"native AVIF helper 输入尺寸过大。"};
    }
    const auto pixel_count = static_cast<std::uint64_t>(image->width) *
                             static_cast<std::uint64_t>(image->height);
    const auto selection = awj::select_avif_encoder_for_current_build(
        awj::AvifEncoderSelectionRequest{.requested_encoder = cfg.encoder,
                                         .requested_chroma = cfg.chroma,
                                         .requested_bit_depth = cfg.bit_depth,
                                         .pixel_count = pixel_count,
                                         .width = static_cast<std::uint32_t>(image->width),
                                         .height = static_cast<std::uint32_t>(image->height),
                                         .speed = cfg.speed},
        cfg.experimental_encoders);
    if (!selection) {
      return std::unexpected{selection.error()};
    }
    const bool helper_can_write_hdr_metadata =
        selection->applied_encoder != awj::AvifEncoderMode::zenrav1e;
    const std::string applied_hdr_metadata = cfg.source_content_light
                                                 ? (helper_can_write_hdr_metadata ? "kept" : "not-written")
                                                 : "none";
    std::unique_ptr<awj::ImageEncoder> encoder;
    if (selection->applied_encoder == awj::AvifEncoderMode::zenrav1e) {
      encoder = std::make_unique<awj::ZenravifImageEncoder>();
    } else {
      encoder = std::make_unique<awj::AvifLibavifImageEncoder>(selection->applied_encoder);
    }
    auto encoded = encoder->encode(
        *image,
        awj::NativeEncodeSettings{.output_format = awj::OutputFormat::avif,
                                   .quality = cfg.quality,
                                   .speed = cfg.speed,
                                   .speed_explicit = true,
                                   .bit_depth = selection->applied_bit_depth,
                                   .chroma_mode = selection->applied_chroma,
                                   .avif_encoder = selection->applied_encoder,
                                   .requested_chroma_mode = selection->requested_chroma,
                                   .requested_avif_encoder = selection->requested_encoder,
                                   .requested_bit_depth = selection->requested_bit_depth,
                                   .source_content_light = cfg.source_content_light,
                                   .source_has_hdr_metadata = cfg.source_content_light.has_value(),
                                   .applied_hdr_metadata = applied_hdr_metadata,
                                   .bit_depth_reason = selection->bit_depth_reason,
                                   .encoder_fallback_reason = selection->fallback_reason,
                                   .resources = awj::ResourcePlan{
                                       .file_parallelism = 1,
                                       .encoder_threads_per_file = cfg.threads,
                                       .global_thread_budget = cfg.threads}});
    if (!encoded) {
      return std::unexpected{encoded.error()};
    }
    return write_bytes(cfg.output, encoded->encoded.bytes);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"native AVIF helper 编码内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"native AVIF helper 编码数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"native AVIF helper 编码文件系统访问失败。"};
  }
}

std::expected<void, std::string> run_decode(const HelperConfig& cfg) {
  try {
    awj::AvifImageDecoder decoder{cfg.threads};
    if (!decoder.can_decode(cfg.input)) {
      return std::unexpected{"decode mode 当前只支持 AVIF 输入。"};
    }
    auto decoded = decoder.decode(cfg.input);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }
    return awj::write_raw_image_file(cfg.output, decoded->image);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"native AVIF helper 解码内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"native AVIF helper 解码数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"native AVIF helper 解码文件系统访问失败。"};
  }
}

std::expected<void, std::string> run_helper(const HelperConfig& cfg) {
  switch (cfg.mode) {
    case HelperMode::decode:
      return run_decode(cfg);
    case HelperMode::encode:
    default:
      return run_encode(cfg);
  }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  try {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
  } catch (const std::exception&) {
    print_line("[FAIL] native AVIF helper 初始化异常。");
    return 1;
  } catch (...) {
    print_line("[FAIL] native AVIF helper 初始化异常: 未知异常");
    return 1;
  }

  std::optional<HelperConfig> cfg;
  try {
    auto parsed = parse_args(argc, argv);
    if (!parsed) {
      print_line(std::format("[FAIL] {}", parsed.error()));
      return 1;
    }
    cfg = std::move(*parsed);

    auto result = run_helper(*cfg);
    if (!result) {
      auto message = awj::redact_path_for_user(result.error(), cfg->input);
      message = awj::redact_path_for_user(std::move(message), cfg->output);
      print_line(std::format("[FAIL] {}", message));
      return 1;
    }
    return 0;
  } catch (const std::bad_alloc&) {
    print_line("[FAIL] native AVIF helper 运行异常: 内存不足。");
    return 1;
  } catch (const std::length_error&) {
    print_line("[FAIL] native AVIF helper 运行异常: 数据超过运行时限制。");
    return 1;
  } catch (const std::exception&) {
    if (!cfg) {
      print_line("[FAIL] native AVIF helper 参数解析异常。");
      return 1;
    }
    print_line("[FAIL] native AVIF helper 运行异常。");
    return 1;
  } catch (...) {
    print_line("[FAIL] native AVIF helper 运行异常: 未知异常");
    return 1;
  }
}
