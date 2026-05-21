#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

import awj.avif_aom_codec;
import awj.config;
import awj.core;
import awj.image;
import awj.jxl_codec;
import awj.raw_image_io;
import awj.resource_planner;
import awj.webp_codec;

namespace {

enum class HelperMode { encode, decode };

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

std::expected<int, std::string> parse_int(std::wstring_view value,
                                          std::string_view name,
                                          int minimum,
                                          int maximum) {
  const auto text = awj::utf8_from_wide(value);
  int parsed{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end || parsed < minimum || parsed > maximum) {
    return std::unexpected{std::format("{} 参数无效。", name)};
  }
  return parsed;
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
  if (text == "svt") {
    return awj::AvifEncoderMode::svt;
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
  bool experimental_encoders{};
};

std::expected<HelperConfig, std::string> parse_args(int argc, wchar_t* argv[]) {
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
}

std::expected<awj::ImageDecodeResult, std::string> decode_input(
    const std::filesystem::path& path) {
  awj::WebPImageDecoder webp;
  if (webp.can_decode(path)) {
    return webp.decode(path);
  }
  awj::JXLImageDecoder jxl;
  if (jxl.can_decode(path)) {
    return jxl.decode(path);
  }
  awj::AvifImageDecoder avif;
  if (avif.can_decode(path)) {
    return avif.decode(path);
  }
  return std::unexpected{std::format("native AVIF helper 不支持输入格式: {}",
                                     awj::path_to_utf8(path.extension()))};
}

std::expected<void, std::string> write_bytes(const std::filesystem::path& path,
                                             const std::vector<std::byte>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return std::unexpected{std::format("无法创建输出目录: {}", ec.message())};
  }
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return std::unexpected{std::format("无法写入输出文件: {}", awj::path_to_utf8(path))};
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    return std::unexpected{std::format("写入输出文件失败: {}", awj::path_to_utf8(path))};
  }
  return {};
}

std::expected<void, std::string> run_encode(const HelperConfig& cfg) {
  auto decoded = decode_input(cfg.input);
  if (!decoded) {
    return std::unexpected{decoded.error()};
  }
  const auto pixel_count = static_cast<std::uint64_t>(decoded->image.width) *
                           static_cast<std::uint64_t>(decoded->image.height);
  const auto selection = awj::select_avif_encoder_for_current_build(
      awj::AvifEncoderSelectionRequest{.requested_encoder = cfg.encoder,
                                       .requested_chroma = cfg.chroma,
                                       .requested_bit_depth = cfg.bit_depth,
                                       .pixel_count = pixel_count,
                                       .speed = cfg.speed},
      cfg.experimental_encoders);
  if (!selection) {
    return std::unexpected{selection.error()};
  }
  std::unique_ptr<awj::ImageEncoder> encoder;
  if (selection->applied_encoder == awj::AvifEncoderMode::zenrav1e) {
    encoder = std::make_unique<awj::ZenravifImageEncoder>();
  } else {
    encoder = std::make_unique<awj::AvifLibavifImageEncoder>(selection->applied_encoder);
  }
  auto encoded = encoder->encode(
      decoded->image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::avif,
                                 .quality = cfg.quality,
                                 .speed = cfg.speed,
                                 .bit_depth = selection->applied_bit_depth,
                                 .chroma_mode = selection->applied_chroma,
                                 .avif_encoder = selection->applied_encoder,
                                 .requested_chroma_mode = selection->requested_chroma,
                                 .requested_avif_encoder = selection->requested_encoder,
                                 .requested_bit_depth = selection->requested_bit_depth,
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
}

std::expected<void, std::string> run_decode(const HelperConfig& cfg) {
  awj::AvifImageDecoder decoder;
  if (!decoder.can_decode(cfg.input)) {
    return std::unexpected{"decode mode 当前只支持 AVIF 输入。"};
  }
  auto decoded = decoder.decode(cfg.input);
  if (!decoded) {
    return std::unexpected{decoded.error()};
  }
  return awj::write_raw_image_file(cfg.output, decoded->image);
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
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  auto cfg = parse_args(argc, argv);
  if (!cfg) {
    print_line(std::format("[FAIL] {}", cfg.error()));
    return 1;
  }
  auto result = run_helper(*cfg);
  if (!result) {
    print_line(std::format("[FAIL] {}", result.error()));
    return 1;
  }
  return 0;
}
