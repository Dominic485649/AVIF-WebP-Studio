module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

export module awj.native_backend;

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.native_visual_search;
import awj.raw_image_io;
import awj.resource_planner;
import awj.visual_quality;
import awj.webp_codec;

export namespace awj {

namespace native_backend_detail {

class UnsupportedDecoder final : public ImageDecoder {
 public:
  explicit UnsupportedDecoder(std::string id) : id_{std::move(id)} {}

  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  [[nodiscard]] bool can_decode(const fs::path&) const override { return false; }
  std::expected<ImageDecodeResult, std::string> decode(const fs::path&) const override {
    return std::unexpected{std::format("native backend 当前不支持该输入解码器: {}", id_)};
  }

 private:
  std::string id_;
};

std::unique_ptr<ImageDecoder> decoder_for_path(const fs::path& path) {
  WebPImageDecoder webp;
  if (webp.can_decode(path)) {
    return std::make_unique<WebPImageDecoder>();
  }
  JXLImageDecoder jxl;
  if (jxl.can_decode(path)) {
    return std::make_unique<JXLImageDecoder>();
  }
  AvifImageDecoder avif;
  if (avif.can_decode(path)) {
    return std::make_unique<AvifImageDecoder>();
  }
  return std::make_unique<UnsupportedDecoder>(path.extension().string());
}

std::unique_ptr<ImageDecoder> decoder_for_output_format(OutputFormat format) {
  switch (format) {
    case OutputFormat::webp:
      return std::make_unique<WebPImageDecoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageDecoder>();
    case OutputFormat::avif:
      return std::make_unique<AvifImageDecoder>();
    default:
      return std::make_unique<UnsupportedDecoder>("avif");
  }
}

std::unique_ptr<ImageEncoder> encoder_for_output_format(OutputFormat format,
                                                        AvifEncoderMode avif_encoder) {
  switch (format) {
    case OutputFormat::webp:
      return std::make_unique<WebPImageEncoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageEncoder>();
    case OutputFormat::avif:
      if (avif_encoder == AvifEncoderMode::zenrav1e) {
        return std::make_unique<ZenravifImageEncoder>();
      }
      return std::make_unique<AvifLibavifImageEncoder>(avif_encoder);
    default:
      return nullptr;
  }
}

std::expected<std::vector<std::byte>, std::string> read_file_bytes(const fs::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 helper 输出文件: {}", path_to_utf8(path))};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0) {
    return std::unexpected{std::format("helper 输出文件为空: {}", path_to_utf8(path))};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) {
    return std::unexpected{std::format("读取 helper 输出文件失败: {}", path_to_utf8(path))};
  }
  return bytes;
}

std::wstring quote_process_argument(std::wstring_view value) {
  std::wstring quoted{L"\""};
  for (const wchar_t ch : value) {
    if (ch == L'\"') {
      quoted += L"\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += L"\"";
  return quoted;
}

std::wstring helper_encoder_argument(AvifEncoderMode mode) {
  return wide_from_utf8(avif_encoder_mode_name(mode));
}

std::wstring helper_chroma_argument(ChromaMode mode) {
  return wide_from_utf8(chroma_mode_name(mode));
}

std::wstring make_helper_command_line(const fs::path& helper,
                                      const fs::path& input,
                                      const fs::path& output,
                                      const NativeEncodeSettings& settings) {
  std::wstring command = quote_process_argument(helper.native());
  const auto append = [&](std::wstring_view value) {
    command.push_back(L' ');
    command += value;
  };
  append(L"--mode");
  append(L"encode");
  append(L"--encoder");
  append(helper_encoder_argument(settings.avif_encoder));
  append(L"--input");
  append(quote_process_argument(input.native()));
  append(L"--output");
  append(quote_process_argument(output.native()));
  append(L"--quality");
  append(std::format(L"{}", settings.quality));
  append(L"--speed");
  append(std::format(L"{}", settings.speed));
  append(L"--bit-depth");
  append(std::format(L"{}", settings.bit_depth.value_or(8)));
  append(L"--chroma");
  append(helper_chroma_argument(settings.chroma_mode));
  append(L"--threads");
  append(std::format(L"{}", std::max(1, settings.resources.encoder_threads_per_file)));
  if (settings.avif_encoder == AvifEncoderMode::zenrav1e) {
    append(L"--experimental-encoders");
  }
  return command;
}

std::expected<void, std::string> run_helper_command(std::wstring command_line) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return std::unexpected{std::format("启动 AVIF helper 失败: {}", win32_error_message(GetLastError()))};
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
  if (wait != WAIT_OBJECT_0) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    return std::unexpected{std::format("等待 AVIF helper 失败: {}", win32_error_message(error))};
  }
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    return std::unexpected{std::format("读取 AVIF helper 退出码失败: {}", win32_error_message(error))};
  }
  CloseHandle(process.hProcess);
  if (exit_code != 0) {
    return std::unexpected{std::format("AVIF helper 编码失败，退出码 {}。", exit_code)};
  }
  return {};
}

std::atomic<std::uint64_t> helper_counter{};

fs::path helper_executable_path() {
  const auto exe_dir = executable_directory();
  const auto internal = exe_dir.parent_path() / L"internal" / exe_dir.filename() /
                        L"AWJ-native-avif-helper.exe";
  std::error_code ec;
  if (fs::exists(internal, ec) && !ec) {
    return internal;
  }
  return exe_dir / L"AWJ-native-avif-helper.exe";
}

std::expected<NativeEncodeResult, std::string> encode_with_helper_process(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 || image.planes.empty()) {
    return std::unexpected{"AVIF helper 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto temp_root = fs::temp_directory_path() / L"awjimage-avif-helper";
  std::error_code ec;
  fs::create_directories(temp_root, ec);
  if (ec) {
    return std::unexpected{std::format("无法创建 AVIF helper 临时目录: {}", ec.message())};
  }
  const auto id = helper_counter.fetch_add(1);
  const auto input_path = temp_root / std::format(L"input-{}-{}.awsraw", GetCurrentProcessId(), id);
  const auto output_path = temp_root / std::format(L"output-{}-{}.avif", GetCurrentProcessId(), id);
  const auto cleanup = [&] {
    std::error_code remove_ec;
    fs::remove(input_path, remove_ec);
    fs::remove(output_path, remove_ec);
  };
  if (auto written = write_raw_image_file(input_path, image); !written) {
    cleanup();
    return std::unexpected{written.error()};
  }
  const auto helper = helper_executable_path();
  if (!fs::exists(helper, ec) || ec) {
    cleanup();
    return std::unexpected{"AVIF helper 缺失；请重新安装或重新构建 AWJimage。"};
  }
  auto command_line = make_helper_command_line(helper, input_path, output_path, settings);
  if (auto ran = run_helper_command(std::move(command_line)); !ran) {
    cleanup();
    return std::unexpected{ran.error()};
  }
  auto bytes = read_file_bytes(output_path);
  cleanup();
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }

  const auto speed_mapping = avif_speed_mapping_for(AvifEncoderSelection{
      .applied_encoder = settings.avif_encoder,
      .speed = settings.speed});
  return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*bytes),
                                                    .codec_name = avif_encoder_mode_name(settings.avif_encoder)},
                            .diagnostics = EncodeDiagnostics{
                                .encoder_id = avif_encoder_mode_name(settings.avif_encoder),
                                .requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder),
                                .requested_chroma = chroma_mode_name(settings.requested_chroma_mode),
                                .applied_chroma = chroma_mode_name(settings.chroma_mode),
                                .requested_bit_depth = settings.requested_bit_depth,
                                .applied_bit_depth = settings.bit_depth,
                                .bit_depth_reason = settings.bit_depth_reason,
                                .fallback_reason = settings.encoder_fallback_reason,
                                .encoder_experimental = settings.avif_encoder == AvifEncoderMode::zenrav1e,
                                .encoder_license = settings.avif_encoder == AvifEncoderMode::zenrav1e
                                                       ? "AGPL-3.0-only OR LicenseRef-Imazen-Commercial"
                                                       : "BSD-2-Clause",
                                .speed_mapping = speed_mapping,
                                .encoder_threads = settings.resources.encoder_threads_per_file,
                                .memory_budget_bytes = settings.resources.memory_limit_bytes},
                            .final_quality = settings.quality,
                            .lossless = settings.quality >= 100,
                            .search_attempt_count = 1};
}

std::expected<void, std::string> write_output_bytes(const fs::path& path,
                                                    std::span<const std::byte> bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                       path_to_utf8(path.parent_path()), ec.message())};
  }
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return std::unexpected{std::format("无法写入输出文件: {}", path_to_utf8(path))};
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    return std::unexpected{std::format("写入输出文件失败: {}", path_to_utf8(path))};
  }
  return {};
}

NativeEncodeSettings settings_from_config(const AppConfig& cfg, ResourcePlan resources) {
  return NativeEncodeSettings{.output_format = cfg.output_format,
                              .quality = cfg.quality,
                              .visual_quality = cfg.visual_quality,
                              .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
                              .bit_depth = cfg.bit_depth,
                              .chroma_mode = cfg.chroma_mode,
                              .avif_encoder = cfg.avif_encoder,
                              .requested_chroma_mode = cfg.chroma_mode,
                              .requested_avif_encoder = cfg.avif_encoder,
                              .requested_bit_depth = cfg.bit_depth,
                              .strip_metadata = cfg.strip_metadata,
                              .visual_quality_fallback = cfg.visual_quality_fallback,
                              .avif_tune_iq = encoding_defaults::default_avif_tune_iq,
                              .resources = resources};
}

void copy_native_result(const NativeEncodeResult& native, EncodeResult& result) {
  result.output_bytes = native.encoded.bytes.size();
  result.final_encoder_quality = native.final_quality;
  result.search_attempt_count = native.search_attempt_count;
  result.lossless = native.lossless;
  result.speed = native.diagnostics.speed_mapping.user_speed;
  result.decoder_id = native.diagnostics.decoder_id;
  result.encoder_id = native.diagnostics.encoder_id;
  result.requested_encoder_id = native.diagnostics.requested_encoder_id;
  result.requested_chroma = native.diagnostics.requested_chroma;
  result.applied_chroma = native.diagnostics.applied_chroma;
  result.requested_bit_depth = native.diagnostics.requested_bit_depth;
  result.applied_bit_depth = native.diagnostics.applied_bit_depth;
  result.bit_depth_reason = native.diagnostics.bit_depth_reason;
  result.fallback_reason = native.diagnostics.fallback_reason;
  result.encoder_experimental = native.diagnostics.encoder_experimental;
  result.encoder_license = native.diagnostics.encoder_license;
  result.speed_parameter_kind = native.diagnostics.speed_mapping.codec_key;
  result.applied_speed = native.diagnostics.speed_mapping.codec_value;
  result.encoder_threads = native.diagnostics.encoder_threads;
  result.memory_budget_bytes = native.diagnostics.memory_budget_bytes;
  if (native.visual_score) {
    result.visual_score = native.visual_score->visual_score;
    result.gmsd_quality_score = native.visual_score->gmsd_quality_score;
    result.msssim_quality_score = native.visual_score->msssim_quality_score;
  }
  result.raw_gmsd = native.raw_gmsd;
  result.raw_ms_ssim = native.raw_ms_ssim;
  result.gmsd_weight = GMSD_WEIGHT;
  result.msssim_weight = MSSSIM_WEIGHT;
  result.command = std::format("native:{}:{} q{} speed={}",
                               result.decoder_id,
                               native.diagnostics.encoder_id,
                               native.final_quality,
                               native.diagnostics.speed_mapping.user_speed);
}

}  // namespace native_backend_detail

export class NativeBackend final {
 public:
  NativeBackend(const AppConfig& cfg, FileLogger& logger, ResourcePlan resources)
      : cfg_{cfg}, logger_{logger}, resources_{resources} {}

  EncodeResult encode(const ImageFile& image,
                      std::stop_token stop_token = {}) const {
    const auto started = std::chrono::steady_clock::now();
    EncodeResult result{.index = image.index,
                        .input_path = image.path,
                        .output_path = output_path_for(cfg_, image),
                        .original_bytes = image.bytes,
                        .quality = cfg_.quality,
                        .requested_visual_quality = cfg_.visual_quality,
                        .gmsd_weight = GMSD_WEIGHT,
                        .msssim_weight = MSSSIM_WEIGHT,
                        .final_encoder_quality = cfg_.quality,
                        .speed = cfg_.speed.value_or(default_speed_for(cfg_.output_format)),
                        .quality_overridden_by_visual_quality = cfg_.visual_quality.has_value()};

    if (stop_token.stop_requested()) {
      result.canceled = true;
      result.message = "任务已取消。";
      return result;
    }

    if (cfg_.collision_mode == CollisionMode::skip && fs::exists(result.output_path)) {
      result.processed = true;
      result.ok = true;
      result.skipped = true;
      result.message = "输出已存在，已跳过。";
      return result;
    }

    auto decoder = native_backend_detail::decoder_for_path(image.path);
    if (!decoder->can_decode(image.path)) {
      result.processed = true;
      result.message = std::format("native backend 暂不支持输入格式: {}",
                                   path_to_utf8(image.path.extension()));
      return result;
    }
    auto decoded = decoder->decode(image.path);
    if (!decoded) {
      result.processed = true;
      result.message = decoded.error();
      return result;
    }

    std::unique_ptr<ImageEncoder> encoder;
    auto settings = native_backend_detail::settings_from_config(cfg_, resources_);
    std::string avif_bit_depth_reason;
    if (cfg_.output_format == OutputFormat::avif) {
      const auto pixel_count = static_cast<std::uint64_t>(decoded->image.width) *
                               static_cast<std::uint64_t>(decoded->image.height);
      const auto selection = select_avif_encoder_for_current_build(AvifEncoderSelectionRequest{
          .requested_encoder = cfg_.avif_encoder,
          .requested_chroma = cfg_.chroma_mode,
          .requested_bit_depth = cfg_.bit_depth,
          .pixel_count = pixel_count,
          .speed = settings.speed},
          cfg_.enable_experimental_encoders);
      if (!selection) {
        result.processed = true;
        result.message = selection.error();
        return result;
      }
      settings.avif_encoder = selection->applied_encoder;
      settings.chroma_mode = selection->applied_chroma;
      settings.bit_depth = selection->applied_bit_depth;
      settings.requested_avif_encoder = selection->requested_encoder;
      settings.requested_chroma_mode = selection->requested_chroma;
      settings.requested_bit_depth = selection->requested_bit_depth;
      settings.bit_depth_reason = selection->bit_depth_reason;
      settings.encoder_fallback_reason = selection->fallback_reason;
      avif_bit_depth_reason = selection->bit_depth_reason;
      if (selection->applied_encoder != AvifEncoderMode::zenrav1e) {
        encoder = native_backend_detail::encoder_for_output_format(cfg_.output_format,
                                                                    selection->applied_encoder);
      }
    } else {
      encoder = native_backend_detail::encoder_for_output_format(cfg_.output_format,
                                                                  cfg_.avif_encoder);
    }
    if (!encoder && !(cfg_.output_format == OutputFormat::avif &&
                      settings.avif_encoder == AvifEncoderMode::zenrav1e)) {
      result.processed = true;
      result.message = std::format("native backend 暂不支持输出格式: {}",
                                   output_format_name(cfg_.output_format));
      return result;
    }
    const auto decoder_id = decoded->decoder_id;
    auto output_decoder = native_backend_detail::decoder_for_output_format(cfg_.output_format);
    const auto candidate_path = result.output_path.parent_path() /
                                (result.output_path.filename().wstring() + L".candidate");

    std::expected<NativeEncodeResult, std::string> encoded;
    const bool use_avif_helper = cfg_.output_format == OutputFormat::avif &&
                                 settings.avif_encoder == AvifEncoderMode::zenrav1e;
    if (cfg_.visual_quality) {
      if (use_avif_helper) {
        result.processed = true;
        result.message = "zenrav1e helper path 当前不支持 visual-quality 搜索；请使用 --quality 或改用 AOM。";
        return result;
      }
      auto search = encode_with_native_visual_quality_search(decoded->image, *encoder,
                                                             *output_decoder, settings,
                                                             candidate_path);
      std::error_code ec;
      fs::remove(candidate_path, ec);
      if (!search) {
        result.processed = true;
        result.message = search.error();
        return result;
      }
      encoded = std::move(search->encode_result);
    } else if (use_avif_helper) {
      encoded = native_backend_detail::encode_with_helper_process(decoded->image, settings);
    } else {
      encoded = encoder->encode(decoded->image, settings);
    }
    if (!encoded) {
      result.processed = true;
      result.message = encoded.error();
      return result;
    }

    if (stop_token.stop_requested()) {
      result.canceled = true;
      result.message = "任务已取消。";
      return result;
    }

    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded->encoded.bytes});
        !written) {
      result.processed = true;
      result.message = written.error();
      return result;
    }

    native_backend_detail::copy_native_result(*encoded, result);
    if (!decoder_id.empty()) {
      result.decoder_id = decoder_id;
      result.command = std::format("native:{}:{} q{} speed={}",
                                   result.decoder_id,
                                   result.encoder_id,
                                   result.final_encoder_quality,
                                   result.speed);
    }
    if (!avif_bit_depth_reason.empty()) {
      result.bit_depth_reason = avif_bit_depth_reason;
    }
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    logger_.info(std::format("native encode ok: {} -> {}",
                             path_to_utf8(result.input_path),
                             path_to_utf8(result.output_path)));
    return result;
  }

 private:
  const AppConfig& cfg_;
  FileLogger& logger_;
  ResourcePlan resources_;
};

}  // namespace awj
