module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

export module awj.native_backend;

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.large_image_plan;
import awj.native_visual_search;
import awj.resource_planner;
import awj.svtav1hdr_codec;
import awj.visual_quality;
import awj.webp_codec;

export namespace awj {

namespace native_backend_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

std::string format_timing_seconds(double seconds) {
  return seconds >= 0.0 ? std::format("{:.3f}", seconds) : std::string{""};
}

std::string format_timing_share(double part, double total) {
  return part >= 0.0 && total > 0.0 ? std::format("{:.1f}", part * 100.0 / total) : std::string{""};
}

template <class Function>
void log_info_noexcept(FileLogger& logger, Function&& message) noexcept {
  try {
    logger.info(message());
  } catch (...) {
  }
}

void merge_stage_timing(NativeEncodeResult& native, const EncodeResult& result) {
  auto& timing = native.diagnostics.timing;
  if (result.decode_seconds >= 0.0) {
    timing.decode_seconds = result.decode_seconds;
  }
  if (result.prepare_seconds >= 0.0) {
    timing.prepare_seconds = result.prepare_seconds;
  }
  if (timing.encode_seconds < 0.0 && result.encode_seconds >= 0.0) {
    timing.encode_seconds = result.encode_seconds;
  }
  if (result.write_seconds >= 0.0) {
    timing.write_seconds = result.write_seconds;
  }
}

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

std::unique_ptr<ImageDecoder> decoder_for_output_format(OutputFormat format, int decode_threads) {
  const auto clamped_decode_threads = std::max(1, decode_threads);
  switch (format) {
    case OutputFormat::webp:
      return std::make_unique<WebPImageDecoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageDecoder>(clamped_decode_threads);
    case OutputFormat::avif:
      return std::make_unique<AvifImageDecoder>(clamped_decode_threads);
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
      if (avif_encoder == AvifEncoderMode::svt) {
        return nullptr;
      }
      return std::make_unique<AvifLibavifImageEncoder>(avif_encoder);
    default:
      return nullptr;
  }
}

struct HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

void remove_file_noexcept(const fs::path& path) noexcept {
  try {
    std::error_code ec;
    fs::remove(path, ec);
  } catch (...) {
  }
}

class TempOutputFile {
 public:
  explicit TempOutputFile(const fs::path& path) noexcept : path_{&path} {}
  ~TempOutputFile() {
    if (active_) {
      remove_file_noexcept(*path_);
    }
  }
  TempOutputFile(const TempOutputFile&) = delete;
  TempOutputFile& operator=(const TempOutputFile&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return *path_; }
  void release() noexcept { active_ = false; }

 private:
  const fs::path* path_{};
  bool active_{true};
};

std::expected<void, std::string> clear_transient_file_attributes(const fs::path& path,
                                                                 std::string_view label) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return std::unexpected{std::format("无法读取{}属性 {}: {}",
                                       label,
                                       display_path_for_user(path),
                                       win32_error_message(error))};
  }
  constexpr DWORD transient_attributes =
      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  const DWORD final_attributes = attributes & ~transient_attributes;
  const DWORD normalized_attributes =
      final_attributes == 0 ? FILE_ATTRIBUTE_NORMAL : final_attributes;
  if (normalized_attributes == attributes) {
    return {};
  }
  if (!SetFileAttributesW(path.c_str(), normalized_attributes)) {
    const auto error = GetLastError();
    return std::unexpected{std::format("无法更新{}属性 {}: {}",
                                       label,
                                       display_path_for_user(path),
                                       win32_error_message(error))};
  }
  return {};
}

struct TempOutputWriteFailure {
  std::string message;
  DWORD create_error{};
};

bool output_temp_name_collision(DWORD error) noexcept {
  return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
}

std::expected<void, TempOutputWriteFailure> write_file_bytes_exclusive(const fs::path& path,
                                                                         std::span<const std::byte> bytes) {
  if (bytes.size() > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("临时输出文件内容超过 20 GiB 运行时上限: {}",
                               display_path_for_user(path))}};
  }
  const auto raw_file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                   nullptr);
  if (raw_file == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("无法创建临时输出文件 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(error)),
        .create_error = error}};
  }
  TempOutputFile cleanup{path};
  UniqueHandle file{raw_file};

  auto remaining = bytes.size();
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(bytes.data());
  while (remaining > 0) {
    const auto chunk = static_cast<DWORD>(
        std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file.get(), cursor, chunk, &written, nullptr)) {
      return std::unexpected{TempOutputWriteFailure{
          .message = std::format("写入临时输出文件失败 {}: {}",
                                 display_path_for_user(path),
                                 win32_error_message(GetLastError()))}};
    }
    if (written == 0) {
      return std::unexpected{TempOutputWriteFailure{
          .message = std::format("写入临时输出文件失败 {}", display_path_for_user(path))}};
    }
    cursor += written;
    remaining -= written;
  }
  if (!FlushFileBuffers(file.get())) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("刷新临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
  const HANDLE file_handle = file.get();
  if (!CloseHandle(file_handle)) {
    return std::unexpected{TempOutputWriteFailure{
        .message = std::format("关闭临时输出文件失败 {}: {}",
                               display_path_for_user(path),
                               win32_error_message(GetLastError()))}};
  }
  file.release();
  cleanup.release();
  return {};
}

bool effective_lossless_requested(int quality, std::optional<int> visual_quality) noexcept {
  return visual_quality ? *visual_quality >= 100 : quality >= 100;
}

bool avif_lossless_requested(const AppConfig& cfg) noexcept {
  return cfg.output_format == OutputFormat::avif &&
         effective_lossless_requested(cfg.quality, cfg.visual_quality);
}

bool jxl_lossless_requested(const AppConfig& cfg) noexcept {
  return cfg.output_format == OutputFormat::jxl &&
         effective_lossless_requested(cfg.quality, cfg.visual_quality);
}

bool avif_lossless_passthrough_source(const fs::path& path) {
  static constexpr std::wstring_view avif_extensions[] = {L".avif"};
  return decoder_common::extension_is_one_of(path, avif_extensions);
}

bool jxl_jpeg_bitstream_source(const fs::path& path) {
  static constexpr std::wstring_view jpeg_extensions[] = {L".jpg", L".jpeg", L".jpe", L".jfif"};
  return decoder_common::extension_is_one_of(path, jpeg_extensions);
}

std::uint16_t read_be_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 8) |
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])));
}

bool is_jpeg_sof_marker(std::uint8_t marker) noexcept {
  switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
      return true;
    default:
      return false;
  }
}

PixelFormat jpeg_pixel_format_from_sampling(std::uint8_t component_count,
                                            std::uint8_t horizontal_factor,
                                            std::uint8_t vertical_factor) noexcept {
  if (component_count == 1) {
    return PixelFormat::gray;
  }
  if (component_count != 3) {
    return PixelFormat::unknown;
  }
  if (horizontal_factor == 1 && vertical_factor == 1) {
    return PixelFormat::yuv444;
  }
  if (horizontal_factor == 2 && vertical_factor == 1) {
    return PixelFormat::yuv422;
  }
  if (horizontal_factor == 2 && vertical_factor == 2) {
    return PixelFormat::yuv420;
  }
  return PixelFormat::unknown;
}

struct JpegBitstreamSourceDiagnostics {
  PixelFormat pixel_format{PixelFormat::unknown};
  std::optional<int> bit_depth{};
  bool has_icc{};
};

JpegBitstreamSourceDiagnostics inspect_jpeg_bitstream_source(
    std::span<const std::byte> bytes) noexcept {
  JpegBitstreamSourceDiagnostics diagnostics{};
  if (bytes.size() < 4 || std::to_integer<std::uint8_t>(bytes[0]) != 0xFF ||
      std::to_integer<std::uint8_t>(bytes[1]) != 0xD8) {
    return diagnostics;
  }

  std::size_t offset = 2;
  while (offset + 3 < bytes.size()) {
    while (offset < bytes.size() && std::to_integer<std::uint8_t>(bytes[offset]) == 0xFF) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      break;
    }

    const auto marker = std::to_integer<std::uint8_t>(bytes[offset++]);
    if (marker == 0xD9 || marker == 0xDA) {
      break;
    }
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      continue;
    }
    if (offset + 2 > bytes.size()) {
      break;
    }

    const auto segment_size = read_be_u16(bytes, offset);
    if (segment_size < 2) {
      break;
    }
    const auto payload_offset = offset + 2;
    const auto payload_size = static_cast<std::size_t>(segment_size - 2);
    if (payload_offset > bytes.size() || payload_size > bytes.size() - payload_offset) {
      break;
    }

    if (marker == 0xE2 && payload_size >= sizeof("ICC_PROFILE") + 2) {
      static constexpr char signature[] = "ICC_PROFILE";
      bool matches = true;
      for (std::size_t i = 0; i < sizeof(signature); ++i) {
        if (std::to_integer<std::uint8_t>(bytes[payload_offset + i]) !=
            static_cast<std::uint8_t>(signature[i])) {
          matches = false;
          break;
        }
      }
      diagnostics.has_icc = diagnostics.has_icc || matches;
    }

    if (is_jpeg_sof_marker(marker) && payload_size >= 6) {
      const auto precision = std::to_integer<std::uint8_t>(bytes[payload_offset]);
      const auto component_count = std::to_integer<std::uint8_t>(bytes[payload_offset + 5]);
      if (payload_size >= 6 + static_cast<std::size_t>(component_count) * 3 && component_count > 0) {
        const auto sampling = std::to_integer<std::uint8_t>(bytes[payload_offset + 7]);
        diagnostics.bit_depth = static_cast<int>(precision);
        diagnostics.pixel_format = jpeg_pixel_format_from_sampling(
            component_count,
            static_cast<std::uint8_t>(sampling >> 4),
            static_cast<std::uint8_t>(sampling & 0x0F));
      }
    }

    offset = payload_offset + payload_size;
  }
  return diagnostics;
}

ChromaMode chroma_from_source_pixel_format(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::yuv420:
      return ChromaMode::yuv420;
    case PixelFormat::yuv422:
      return ChromaMode::yuv422;
    case PixelFormat::yuv444:
      return ChromaMode::yuv444;
    case PixelFormat::gray:
    case PixelFormat::rgb:
    case PixelFormat::rgba:
    case PixelFormat::unknown:
    default:
      return ChromaMode::auto_keep;
  }
}

std::string alpha_mode_name(AlphaMode mode) {
  switch (mode) {
    case AlphaMode::straight:
      return "straight";
    case AlphaMode::premultiplied:
      return "premultiplied";
    case AlphaMode::none:
    default:
      return "none";
  }
}

bool image_has_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  return std::ranges::find_if(image.metadata, [kind](const MetadataBlock& block) {
           return block.kind == kind && !block.bytes.empty();
         }) != image.metadata.end();
}

std::string chroma_name_from_pixel_format(PixelFormat pixel_format) {
  const auto chroma = chroma_from_source_pixel_format(pixel_format);
  return chroma == ChromaMode::auto_keep ? "unknown" : chroma_mode_name(chroma);
}

std::string source_chroma_name(const ImageBuffer& image) {
  if (!image.source_info) {
    return "unknown";
  }
  return chroma_name_from_pixel_format(image.source_info->pixel_format);
}

ChromaMode lossless_source_chroma(const ImageBuffer& image) noexcept {
  if (image.source_info) {
    return chroma_from_source_pixel_format(image.source_info->pixel_format);
  }
  return ChromaMode::auto_keep;
}

std::optional<int> lossless_source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    if (image.source_info->bit_depth < 8 && image.bit_depth >= 8) {
      return image.bit_depth;
    }
    return image.source_info->bit_depth;
  }
  return image.bit_depth > 0 ? std::optional<int>{image.bit_depth} : std::nullopt;
}

bool lossless_uses_decoded_bit_depth(const ImageBuffer& image) noexcept {
  return image.source_info && image.source_info->bit_depth > 0 &&
         image.source_info->bit_depth < 8 && image.bit_depth >= 8;
}

std::optional<int> source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    return image.source_info->bit_depth;
  }
  return image.bit_depth > 0 ? std::optional<int>{image.bit_depth} : std::nullopt;
}

std::optional<int> choose_color_value(std::optional<int> user_value,
                                      std::optional<int> source_value) noexcept {
  return user_value ? user_value : source_value;
}

std::optional<int> non_unspecified_color_value(std::optional<int> value, int unspecified) noexcept {
  if (!value || *value == unspecified) {
    return {};
  }
  return value;
}

bool has_user_cicp_settings(const AppConfig& cfg) noexcept {
  return cfg.color_primaries || cfg.transfer_characteristics || cfg.matrix_coefficients ||
         cfg.color_range;
}

bool has_user_hdr_settings(const AppConfig& cfg) noexcept {
  return !cfg.mastering_display.empty() || !cfg.content_light.empty();
}

bool has_user_color_settings(const AppConfig& cfg) noexcept {
  return has_user_cicp_settings(cfg) || has_user_hdr_settings(cfg);
}

bool avif_lossless_passthrough_allowed(const AppConfig& cfg,
                                       const fs::path& path) {
  return avif_lossless_requested(cfg) && avif_lossless_passthrough_source(path) &&
         cfg.avif_encoder == AvifEncoderMode::automatic && cfg.chroma_mode == ChromaMode::auto_keep &&
         !cfg.bit_depth && cfg.alpha_policy != AlphaModePolicy::off && !cfg.strip_metadata &&
         !has_user_color_settings(cfg);
}

bool jxl_jpeg_bitstream_transcode_allowed(const AppConfig& cfg,
                                          const fs::path& path) {
  return jxl_lossless_requested(cfg) && jxl_jpeg_bitstream_source(path) &&
         !cfg.strip_metadata && !has_user_color_settings(cfg);
}

bool encoder_supports_alpha(AvifEncoderMode mode) noexcept {
  return mode == AvifEncoderMode::aom || mode == AvifEncoderMode::zenrav1e;
}

bool encoder_supports_alpha(OutputFormat format) noexcept {
  return format == OutputFormat::jxl || format == OutputFormat::webp;
}

bool alpha_must_be_preserved(AlphaModePolicy policy,
                             bool source_has_alpha_channel,
                             bool has_non_opaque_alpha) noexcept {
  return source_has_alpha_channel &&
         (policy == AlphaModePolicy::force ||
          (policy == AlphaModePolicy::automatic && has_non_opaque_alpha));
}

std::string applied_alpha_name(bool source_has_alpha_channel,
                               bool preserve_alpha,
                               bool supports_alpha) {
  if (!source_has_alpha_channel) {
    return "none";
  }
  if (preserve_alpha && supports_alpha) {
    return "kept";
  }
  return "stripped";
}

std::expected<void, std::string> populate_regular_alpha_decision(
    NativeEncodeSettings& settings,
    const ImageBuffer& image,
    const AppConfig& cfg) {
  const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(image,
                                                                         "native encoder");
  if (!has_non_opaque_alpha) {
    return std::unexpected{has_non_opaque_alpha.error()};
  }
  settings.has_non_opaque_alpha = *has_non_opaque_alpha;
  settings.encoder_supports_alpha = encoder_supports_alpha(cfg.output_format);
  const bool preserve_alpha = settings.source_has_alpha_channel && settings.encoder_supports_alpha &&
                              (cfg.alpha_policy == AlphaModePolicy::force ||
                               (cfg.alpha_policy == AlphaModePolicy::automatic &&
                                *has_non_opaque_alpha));
  settings.applied_alpha = applied_alpha_name(settings.source_has_alpha_channel,
                                              preserve_alpha,
                                              settings.encoder_supports_alpha);
  if (!settings.source_has_alpha_channel) {
    settings.alpha_reason = "源图没有 alpha 通道";
  } else if (cfg.alpha_policy == AlphaModePolicy::off) {
    settings.alpha_reason = "用户请求移除 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic && !*has_non_opaque_alpha) {
    settings.alpha_reason = "auto 移除全不透明 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic && settings.encoder_supports_alpha) {
    settings.alpha_reason = "auto 保留非不透明 alpha，因为当前编码器支持 alpha";
  } else if (cfg.alpha_policy == AlphaModePolicy::automatic) {
    settings.alpha_reason = "auto 移除 alpha，因为当前编码器不支持 alpha";
  } else if (settings.encoder_supports_alpha) {
    settings.alpha_reason = "force 保留源图 alpha 通道";
  } else {
    settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
  }
  return {};
}

void populate_source_image_diagnostics(NativeEncodeSettings& settings,
                                       const ImageBuffer& image) {
  settings.source_chroma = source_chroma_name(image);
  settings.source_bit_depth = source_bit_depth(image);
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.source_has_alpha_channel = image.alpha_mode != AlphaMode::none;
  settings.source_alpha_mode = alpha_mode_name(image.alpha_mode);
  settings.source_has_icc = image_has_metadata(image, MetadataKind::icc);
  settings.source_has_hdr_metadata = image.source_info ? image.source_info->has_hdr_metadata : false;
  if (image.source_info) {
    settings.source_color_primaries = image.source_info->color_primaries;
    settings.source_transfer_characteristics = image.source_info->transfer_characteristics;
    settings.source_matrix_coefficients = image.source_info->matrix_coefficients;
    settings.source_color_range = image.source_info->color_range;
    settings.source_content_light = image.source_info->content_light;
    if (!image.source_info->color_metadata_source.empty()) {
      settings.color_metadata_source = image.source_info->color_metadata_source;
    }
  }
}

void populate_regular_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  if (cfg.strip_metadata) {
    settings.applied_icc = settings.source_has_icc ? "stripped" : "none";
    settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "stripped" : "none";
    settings.color_metadata_source = "stripped";
    settings.color_reason = "用户请求移除元数据";
    return;
  }

  settings.applied_icc = settings.source_has_icc ? "kept" : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "not-written" : "none";
  if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "使用源图 ICC profile 写入编码器元数据";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = settings.source_has_hdr_metadata || settings.source_color_primaries ||
                                    settings.source_transfer_characteristics ||
                                    settings.source_matrix_coefficients || settings.source_color_range
                                ? "当前编码器未写入源图 CICP/HDR 元数据，使用编码器默认值"
                                : "源图色彩元数据未知，使用编码器默认值";
  }
}

void populate_jxl_jpeg_bitstream_source_diagnostics(NativeEncodeSettings& settings,
                                                    std::span<const std::byte> jpeg_bytes,
                                                    const AppConfig& cfg) {
  const auto source = inspect_jpeg_bitstream_source(jpeg_bytes);
  settings.source_chroma = chroma_name_from_pixel_format(source.pixel_format);
  settings.source_bit_depth = source.bit_depth;
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.source_has_alpha_channel = false;
  settings.source_alpha_mode = alpha_mode_name(AlphaMode::none);
  settings.has_non_opaque_alpha = false;
  settings.encoder_supports_alpha = encoder_supports_alpha(cfg.output_format);
  settings.applied_alpha = applied_alpha_name(false, false, settings.encoder_supports_alpha);
  settings.alpha_reason = "源图没有 alpha 通道";
  settings.source_has_icc = source.has_icc;
  settings.source_has_hdr_metadata = false;
  populate_regular_color_decision(settings, cfg);
  settings.chroma_reason = "无损转封装保留 JPEG 码流 chroma";
  settings.bit_depth_reason = "无损转封装保留 JPEG 码流 bit-depth";
  settings.color_metadata_source = settings.source_has_icc ? "source-icc" : "jpeg-bitstream";
  settings.color_reason = settings.source_has_icc
                              ? "无损转封装保留 JPEG 码流 ICC profile"
                              : "无损转封装保留 JPEG 码流元数据";
}

void populate_source_diagnostics(NativeEncodeSettings& settings,
                                 const ImageBuffer& image,
                                 AvifEncoderMode user_encoder) {
  settings.user_encoder_id = avif_encoder_mode_name(user_encoder);
  settings.user_chroma = chroma_mode_name(settings.requested_chroma_mode);
  populate_source_image_diagnostics(settings, image);
}

bool source_hdr_content_light_kept(const NativeEncodeSettings& settings,
                                   bool strip_metadata,
                                   bool user_color_settings) noexcept {
  return settings.source_content_light && !strip_metadata && !user_color_settings;
}

bool source_hdr_cicp_kept(const NativeEncodeSettings& settings,
                          bool strip_metadata,
                          bool user_color_settings) noexcept {
  return !strip_metadata && !user_color_settings &&
         (settings.source_color_primaries || settings.source_transfer_characteristics ||
          settings.source_matrix_coefficients || settings.source_color_range);
}

std::string applied_hdr_metadata_name(const NativeEncodeSettings& settings,
                                      bool strip_metadata,
                                      bool user_color_settings) {
  if (!settings.source_has_hdr_metadata) {
    return "none";
  }
  if (strip_metadata) {
    return "stripped";
  }
  if (source_hdr_content_light_kept(settings, strip_metadata, user_color_settings) ||
      source_hdr_cicp_kept(settings, strip_metadata, user_color_settings)) {
    return "kept";
  }
  return "not-written";
}

std::string applied_hdr_metadata_name(const NativeEncodeSettings& settings,
                                      const AppConfig& cfg) {
  return applied_hdr_metadata_name(settings, cfg.strip_metadata, has_user_color_settings(cfg));
}

void ignore_svt_only_hdr_for_non_svt_encoder(NativeEncodeSettings& settings,
                                             const AppConfig& cfg) {
  if (!has_user_hdr_settings(cfg) || has_user_cicp_settings(cfg)) {
    return;
  }

  settings.applied_icc = settings.source_has_icc
                             ? (cfg.strip_metadata ? "stripped" : "kept")
                             : "none";
  settings.applied_hdr_metadata = applied_hdr_metadata_name(settings, cfg.strip_metadata, false);
  if (cfg.strip_metadata) {
    settings.color_metadata_source = "stripped";
    settings.color_reason = "用户请求移除元数据";
  } else if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "使用源图 ICC profile 写入编码器元数据";
  } else if (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
             settings.applied_matrix_coefficients || settings.applied_color_range) {
    if (settings.color_metadata_source.empty() ||
        settings.color_metadata_source == "user-svt-settings") {
      settings.color_metadata_source = "source-cicp";
    }
    settings.color_reason = "使用源图 CICP 字段写入编码器设置";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = "源图色彩元数据未知，使用编码器默认值";
  }
}

void populate_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  if (cfg.strip_metadata) {
    settings.applied_color_primaries = cfg.color_primaries;
    settings.applied_transfer_characteristics = cfg.transfer_characteristics;
    settings.applied_matrix_coefficients = cfg.matrix_coefficients;
    settings.applied_color_range = cfg.color_range;
    settings.svtav1hdr.color_primaries = cfg.color_primaries;
    settings.svtav1hdr.transfer_characteristics = cfg.transfer_characteristics;
    settings.svtav1hdr.matrix_coefficients = cfg.matrix_coefficients;
    settings.svtav1hdr.color_range = cfg.color_range;
    settings.applied_icc = settings.source_has_icc ? "stripped" : "none";
    settings.applied_hdr_metadata = applied_hdr_metadata_name(settings, cfg);
    if (has_user_color_settings(cfg)) {
      settings.color_metadata_source = "user-svt-settings";
      settings.color_reason = "已移除源图元数据，并使用用户 color/HDR 设置";
    } else {
      settings.color_metadata_source = "stripped";
      settings.color_reason = "用户请求移除元数据";
    }
    return;
  }

  settings.applied_color_primaries = choose_color_value(cfg.color_primaries,
                                                        settings.source_color_primaries);
  settings.applied_transfer_characteristics = choose_color_value(cfg.transfer_characteristics,
                                                                 settings.source_transfer_characteristics);
  settings.applied_matrix_coefficients = choose_color_value(cfg.matrix_coefficients,
                                                            settings.source_matrix_coefficients);
  settings.applied_color_range = choose_color_value(cfg.color_range,
                                                    settings.source_color_range);
  settings.svtav1hdr.color_primaries = settings.applied_color_primaries;
  settings.svtav1hdr.transfer_characteristics = settings.applied_transfer_characteristics;
  settings.svtav1hdr.matrix_coefficients = settings.applied_matrix_coefficients;
  settings.svtav1hdr.color_range = settings.applied_color_range;

  const bool user_color_settings = has_user_color_settings(cfg);
  settings.applied_icc = settings.source_has_icc
                             ? (user_color_settings ? "not-written" : "kept")
                             : "none";
  settings.applied_hdr_metadata = applied_hdr_metadata_name(settings, cfg);
  if (user_color_settings) {
    settings.color_metadata_source = "user-svt-settings";
    settings.color_reason = "用户 color/HDR 设置覆盖源图元数据";
  } else if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "使用源图 ICC profile 写入编码器元数据";
  } else if (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
             settings.applied_matrix_coefficients || settings.applied_color_range) {
    if (settings.color_metadata_source.empty()) {
      settings.color_metadata_source = "source-cicp";
    }
    settings.color_reason = "使用源图 CICP 字段写入编码器设置";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = "源图色彩元数据未知，使用编码器默认值";
  }
}

void populate_applied_avif_color_diagnostics(NativeEncodeSettings& settings,
                                             const AppConfig& cfg,
                                             AvifEncoderMode encoder,
                                             ChromaMode chroma,
                                             bool lossless) {
  if (encoder == AvifEncoderMode::zenrav1e) {
    if (settings.applied_icc == "kept") {
      settings.applied_icc = settings.source_has_icc ? "not-written" : "none";
    }
    if (settings.applied_hdr_metadata == "kept") {
      settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "not-written" : "none";
    }
    settings.color_metadata_source = "zenravif-bridge-default";
    settings.color_reason = "zenravif bridge 未暴露 CICP/HDR 元数据控制";
    return;
  }

  if (encoder == AvifEncoderMode::svt) {
    settings.applied_color_primaries = settings.svtav1hdr.color_primaries.value_or(1);
    settings.applied_transfer_characteristics = settings.svtav1hdr.transfer_characteristics.value_or(13);
    settings.applied_matrix_coefficients = settings.svtav1hdr.matrix_coefficients.value_or(1);
    settings.applied_color_range = settings.svtav1hdr.color_range.value_or(1);
    if (has_user_hdr_settings(cfg)) {
      settings.applied_hdr_metadata = "user-svt-settings";
    }
    if (settings.color_metadata_source == "stripped") {
      settings.color_metadata_source = "svt-encoder-default";
      settings.color_reason = "已移除源图元数据，并使用 svt-av1-hdr 默认值";
    } else if (settings.color_metadata_source == "encoder-default") {
      settings.color_metadata_source = "svt-encoder-default";
      settings.color_reason = "源图色彩元数据未知，使用 svt-av1-hdr 默认值";
    }
    return;
  }

  const bool user_cicp_settings = has_user_cicp_settings(cfg);
  ignore_svt_only_hdr_for_non_svt_encoder(settings, cfg);
  if (user_cicp_settings && settings.color_metadata_source == "user-svt-settings") {
    settings.color_metadata_source = "user-cicp-settings";
    settings.color_reason = cfg.strip_metadata
                                ? "已移除源图元数据，并使用用户 CICP 设置"
                                : "用户 CICP 设置覆盖源图元数据";
  }

  settings.applied_color_primaries = cfg.color_primaries
                                          ? cfg.color_primaries
                                          : non_unspecified_color_value(
                                                settings.applied_color_primaries, 2);
  settings.applied_transfer_characteristics = cfg.transfer_characteristics
                                                 ? cfg.transfer_characteristics
                                                 : non_unspecified_color_value(
                                                       settings.applied_transfer_characteristics, 2);
  settings.applied_matrix_coefficients = cfg.matrix_coefficients
                                            ? cfg.matrix_coefficients
                                            : non_unspecified_color_value(
                                                  settings.applied_matrix_coefficients, 2);
  settings.applied_color_range = settings.applied_color_range.value_or(1);

  if (!settings.applied_color_primaries && !lossless && settings.applied_icc != "kept") {
    settings.applied_color_primaries = 1;
  }
  if (!settings.applied_transfer_characteristics && !lossless && settings.applied_icc != "kept") {
    settings.applied_transfer_characteristics = 13;
  }
  if (!settings.applied_matrix_coefficients) {
    if (lossless && chroma == ChromaMode::yuv444) {
      settings.applied_matrix_coefficients = 0;
    } else if (!lossless && settings.applied_icc != "kept") {
      settings.applied_matrix_coefficients = 1;
    }
  }

  if (lossless && chroma == ChromaMode::yuv444 && !cfg.matrix_coefficients &&
      !settings.source_matrix_coefficients) {
    settings.color_metadata_source = user_cicp_settings ? "user-cicp-settings" : "aom-lossless-transform";
    settings.color_reason = "无损 yuv444 使用 identity matrix 执行 RGB/YUV 转换，因为源图 matrix 未指定";
  } else if (settings.color_metadata_source == "stripped" &&
             (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
              settings.applied_matrix_coefficients || settings.applied_color_range)) {
    settings.color_metadata_source = user_cicp_settings ? "user-cicp-settings" : "aom-encoder-default";
    settings.color_reason = user_cicp_settings
                                ? "已移除源图元数据，并使用用户 CICP 设置"
                                : "已移除源图元数据，并使用 AOM/libavif 默认值";
  } else if (settings.color_metadata_source == "encoder-default") {
    settings.color_metadata_source = "aom-encoder-default";
    settings.color_reason = "源图色彩元数据未知，使用 AOM/libavif 默认值";
  }
}

bool avif_lossless_bit_depth_supported(int bit_depth) noexcept {
  return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

std::atomic<std::uint64_t> output_temp_counter{};
constexpr int kMaxOutputTempPathAttempts = 1000;

fs::path make_output_temp_path(const fs::path& target) {
  const auto parent = target.parent_path();
  const auto id = output_temp_counter.fetch_add(1, std::memory_order_relaxed);
  return parent / std::format(L".awj-output-{}-{}.tmp", GetCurrentProcessId(), id);
}

std::expected<void, std::string> write_output_bytes(const fs::path& path,
                                                    std::span<const std::byte> bytes,
                                                    bool replace_existing,
                                                    std::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (bytes.empty()) {
      return std::unexpected{std::format("输出内容为空，无法写入输出文件: {}", display_path_for_user(path))};
    }
    if (bytes.size() > encoding_defaults::max_input_file_bytes) {
      return std::unexpected{std::format("输出内容超过 20 GiB 运行时上限，无法写入输出文件: {}",
                                        display_path_for_user(path))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    std::optional<fs::path> temp_path;
    for (int attempt = 0; attempt < kMaxOutputTempPathAttempts; ++attempt) {
      auto candidate = make_output_temp_path(path);
      if (normalized_lower_path_key(candidate) == normalized_lower_path_key(path)) {
        continue;
      }
      if (auto written = write_file_bytes_exclusive(candidate, bytes); written) {
        temp_path = std::move(candidate);
        break;
      } else {
        auto failure = std::move(written.error());
        if (!output_temp_name_collision(failure.create_error)) {
          return std::unexpected{std::move(failure.message)};
        }
      }
    }
    if (!temp_path) {
      return std::unexpected{std::format("无法创建唯一临时输出路径: {}",
                                         display_path_for_user(path))};
    }
    TempOutputFile temp{*temp_path};
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return std::unexpected{attributes.error()};
    }
    const DWORD move_flags = replace_existing
                                 ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                 : MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(temp_path->c_str(), path.c_str(), move_flags)) {
      const auto error = GetLastError();
      if (!replace_existing &&
          (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
        return std::unexpected{std::format("输出路径已存在，未覆盖 {}", display_path_for_user(path))};
      }
      return std::unexpected{std::format("替换输出文件失败 {}: {}", display_path_for_user(path),
                                         win32_error_message(error))};
    }
    temp.release();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"写入输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"写入输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"写入输出文件时文件系统访问失败。"};
  }
}

std::expected<std::uintmax_t, std::string> copy_file_to_output(const fs::path& source,
                                                               const fs::path& path,
                                                               bool replace_existing,
                                                               std::stop_token stop_token = {}) {
  try {
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    std::error_code source_size_error;
    const auto source_size = fs::file_size(source, source_size_error);
    if (source_size_error) {
      return std::unexpected{std::format("读取 AVIF 直通输入文件大小失败 {}: {}",
                                         display_path_for_user(source),
                                         source_size_error.message())};
    }
    if (source_size == 0) {
      return std::unexpected{std::format("AVIF 直通输入文件为空: {}",
                                         display_path_for_user(source))};
    }
    if (source_size > encoding_defaults::max_input_file_bytes) {
      return std::unexpected{std::format("AVIF 直通输入超过 20 GiB 输入上限: {}",
                                         display_path_for_user(source))};
    }
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    std::uintmax_t copied_bytes = 0;
    std::optional<fs::path> temp_path;
    std::optional<TempOutputFile> temp;
    {
      const HANDLE raw_source = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                           nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                           nullptr);
      if (raw_source == INVALID_HANDLE_VALUE) {
        return std::unexpected{std::format("无法读取 AVIF 直通输入文件 {}: {}",
                                           display_path_for_user(source),
                                           win32_error_message(GetLastError()))};
      }
      UniqueHandle source_file{raw_source};

      UniqueHandle output_file;
      for (int attempt = 0; attempt < kMaxOutputTempPathAttempts; ++attempt) {
        auto candidate = make_output_temp_path(path);
        if (normalized_lower_path_key(candidate) == normalized_lower_path_key(path)) {
          continue;
        }
        const HANDLE raw_output = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                             CREATE_NEW,
                                             FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                             nullptr);
        if (raw_output == INVALID_HANDLE_VALUE) {
          const auto error = GetLastError();
          if (output_temp_name_collision(error)) {
            continue;
          }
          return std::unexpected{std::format("无法创建临时输出文件 {}: {}",
                                             display_path_for_user(candidate),
                                             win32_error_message(error))};
        }
        output_file.reset(raw_output);
        temp_path = std::move(candidate);
        temp.emplace(*temp_path);
        break;
      }
      if (!temp_path || !output_file) {
        return std::unexpected{std::format("无法创建唯一临时输出路径: {}",
                                           display_path_for_user(path))};
      }

      std::vector<std::byte> buffer(1024 * 1024);
      while (true) {
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
        DWORD read = 0;
        if (!ReadFile(source_file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
          return std::unexpected{std::format("读取 AVIF 直通输入文件失败 {}: {}",
                                             display_path_for_user(source),
                                             win32_error_message(GetLastError()))};
        }
        if (read == 0) {
          break;
        }
        if (copied_bytes > encoding_defaults::max_input_file_bytes - read) {
          return std::unexpected{std::format("AVIF 直通输入超过 20 GiB 输入上限: {}",
                                             display_path_for_user(source))};
        }
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(buffer.data());
        DWORD remaining = read;
        while (remaining > 0) {
          DWORD written = 0;
          if (!WriteFile(output_file.get(), cursor, remaining, &written, nullptr)) {
            return std::unexpected{std::format("写入临时输出文件失败 {}: {}",
                                               display_path_for_user(*temp_path),
                                               win32_error_message(GetLastError()))};
          }
          if (written == 0) {
            return std::unexpected{std::format("写入临时输出文件失败 {}", display_path_for_user(*temp_path))};
          }
          cursor += written;
          remaining -= written;
        }
        copied_bytes += read;
      }
      if (copied_bytes == 0) {
        return std::unexpected{std::format("AVIF 直通输入文件为空: {}", display_path_for_user(source))};
      }
      if (!FlushFileBuffers(output_file.get())) {
        return std::unexpected{std::format("刷新临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
      const HANDLE output_handle = output_file.get();
      if (!CloseHandle(output_handle)) {
        return std::unexpected{std::format("关闭临时输出文件失败 {}: {}",
                                           display_path_for_user(*temp_path),
                                           win32_error_message(GetLastError()))};
      }
      output_file.release();
    }

    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    if (auto attributes = clear_transient_file_attributes(*temp_path, "临时输出文件"); !attributes) {
      return std::unexpected{attributes.error()};
    }
    const DWORD move_flags = replace_existing
                                 ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                 : MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(temp_path->c_str(), path.c_str(), move_flags)) {
      const auto error = GetLastError();
      if (!replace_existing &&
          (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
        return std::unexpected{std::format("输出路径已存在，未覆盖 {}", display_path_for_user(path))};
      }
      return std::unexpected{std::format("替换输出文件失败 {}: {}", display_path_for_user(path),
                                         win32_error_message(error))};
    }
    temp->release();
    return copied_bytes;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"复制输出文件时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"复制输出文件路径超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"复制输出文件时文件系统访问失败。"};
  }
}

NativeEncodeSettings settings_from_config(const AppConfig& cfg, ResourcePlan resources) {
  return NativeEncodeSettings{.output_format = cfg.output_format,
                              .quality = cfg.quality,
                              .visual_quality = cfg.visual_quality,
                              .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
                              .speed_explicit = cfg.speed.has_value(),
                              .bit_depth = cfg.bit_depth,
                              .bit_depth_explicit = cfg.bit_depth.has_value(),
                              .chroma_mode = cfg.chroma_mode,
                              .avif_encoder = cfg.avif_encoder,
                              .alpha_policy = cfg.alpha_policy,
                              .requested_chroma_mode = cfg.chroma_mode,
                              .requested_avif_encoder = cfg.avif_encoder,
                              .requested_alpha_policy = cfg.alpha_policy,
                              .requested_bit_depth = cfg.bit_depth,
                              .strip_metadata = cfg.strip_metadata,
                              .visual_quality_fallback = cfg.visual_quality_fallback,
                              .visual_quality_gpu = cfg.visual_quality_gpu,
                              .jxl_jpeg_lossless_candidate = false,
                              .avif_tune_iq = encoding_defaults::default_avif_tune_iq,
                              .svtav1hdr = SvtAv1HdrSettings{.crf = cfg.svtav1hdr_crf,
                                                            .preset = cfg.svtav1hdr_preset.value_or(encoding_defaults::default_svtav1hdr_preset),
                                                            .tune = cfg.svtav1hdr_tune,
                                                            .keyint = cfg.svtav1hdr_keyint.value_or(encoding_defaults::default_svtav1hdr_keyint),
                                                            .avif = encoding_defaults::default_svtav1hdr_avif,
                                                            .params = cfg.svtav1hdr_params,
                                                            .color_primaries = cfg.color_primaries,
                                                            .transfer_characteristics = cfg.transfer_characteristics,
                                                            .matrix_coefficients = cfg.matrix_coefficients,
                                                            .color_range = cfg.color_range,
                                                            .mastering_display = cfg.mastering_display,
                                                            .content_light = cfg.content_light},
                              .resources = resources};
}

void copy_native_result(const NativeEncodeResult& native, EncodeResult& result) {
  result.output_bytes = native.encoded.bytes.size();
  result.final_encoder_quality = native.final_quality;
  result.visual_quality_target_met = native.visual_quality_target_met;
  result.search_attempt_count = native.search_attempt_count;
  result.lossless = native.lossless;
  result.speed = native.diagnostics.speed_mapping.user_speed;
  result.decoder_id = native.diagnostics.decoder_id;
  result.encoder_id = native.diagnostics.encoder_id;
  result.requested_encoder_id = native.diagnostics.requested_encoder_id;
  result.user_encoder_id = native.diagnostics.user_encoder_id;
  result.user_chroma = native.diagnostics.user_chroma;
  result.source_chroma = native.diagnostics.source_chroma;
  result.requested_chroma = native.diagnostics.requested_chroma;
  result.applied_chroma = native.diagnostics.applied_chroma;
  result.chroma_reason = native.diagnostics.chroma_reason;
  result.source_bit_depth = native.diagnostics.source_bit_depth;
  result.requested_bit_depth = native.diagnostics.requested_bit_depth;
  result.applied_bit_depth = native.diagnostics.applied_bit_depth;
  result.bit_depth_reason = native.diagnostics.bit_depth_reason;
  result.alpha_policy = native.diagnostics.alpha_policy;
  result.source_has_alpha_channel = native.diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = native.diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = native.diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = native.diagnostics.encoder_supports_alpha;
  result.applied_alpha = native.diagnostics.applied_alpha;
  result.alpha_reason = native.diagnostics.alpha_reason;
  result.source_color_primaries = native.diagnostics.source_color_primaries;
  result.source_transfer_characteristics = native.diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = native.diagnostics.source_matrix_coefficients;
  result.source_color_range = native.diagnostics.source_color_range;
  result.applied_color_primaries = native.diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = native.diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = native.diagnostics.applied_matrix_coefficients;
  result.applied_color_range = native.diagnostics.applied_color_range;
  result.source_has_icc = native.diagnostics.source_has_icc;
  result.applied_icc = native.diagnostics.applied_icc;
  result.source_has_hdr_metadata = native.diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = native.diagnostics.applied_hdr_metadata;
  result.color_metadata_source = native.diagnostics.color_metadata_source;
  result.color_reason = native.diagnostics.color_reason;
  result.fallback_reason = native.diagnostics.fallback_reason;
  result.used_decoder_fallback = native.diagnostics.used_decoder_fallback;
  result.visual_quality_gpu_requested = native.diagnostics.visual_quality_gpu_requested;
  result.visual_quality_gpu_used = native.diagnostics.visual_quality_gpu_used;
  result.visual_quality_gpu_path = native.diagnostics.visual_quality_gpu_path;
  result.visual_quality_gpu_fallback_reason = native.diagnostics.visual_quality_gpu_fallback_reason;
  result.encoder_experimental = native.diagnostics.encoder_experimental;
  result.encoder_license = native.diagnostics.encoder_license;
  result.integration_mode = native.diagnostics.integration_mode;
  result.svtav1hdr_helper_path = native.diagnostics.svtav1hdr_helper_path;
  result.svtav1hdr_crf = native.diagnostics.svtav1hdr_crf;
  result.svtav1hdr_preset = native.diagnostics.svtav1hdr_preset;
  result.svtav1hdr_tune = native.diagnostics.svtav1hdr_tune;
  result.svtav1hdr_keyint = native.diagnostics.svtav1hdr_keyint;
  result.svtav1hdr_hdr_metadata = native.diagnostics.svtav1hdr_hdr_metadata;
  result.svtav1hdr_note = native.diagnostics.svtav1hdr_note;
  result.speed_parameter_kind = native.diagnostics.speed_mapping.codec_key;
  result.applied_speed = native.diagnostics.speed_mapping.codec_value;
  result.encoder_threads = native.diagnostics.encoder_threads;
  result.memory_budget_bytes = native.diagnostics.memory_budget_bytes;
  result.decode_seconds = native.diagnostics.timing.decode_seconds;
  result.prepare_seconds = native.diagnostics.timing.prepare_seconds;
  result.encode_seconds = native.diagnostics.timing.encode_seconds;
  result.write_seconds = native.diagnostics.timing.write_seconds;
  result.visual_quality_search_seconds = native.diagnostics.timing.visual_quality_search_seconds;
  result.visual_quality_candidate_encode_seconds = native.diagnostics.timing.visual_quality_candidate_encode_seconds;
  result.visual_quality_candidate_decode_seconds = native.diagnostics.timing.visual_quality_candidate_decode_seconds;
  result.visual_quality_candidate_io_seconds = native.diagnostics.timing.visual_quality_candidate_io_seconds;
  result.visual_quality_luma_seconds = native.diagnostics.timing.visual_quality_luma_seconds;
  result.gmsd_seconds = native.diagnostics.timing.gmsd_seconds;
  result.ms_ssim_seconds = native.diagnostics.timing.ms_ssim_seconds;
  result.visual_quality_metric_seconds = native.diagnostics.timing.visual_quality_metric_seconds;
  result.visual_quality_candidate_count = native.diagnostics.timing.visual_quality_candidate_count;
  result.visual_quality_decode_memory_fallback_count = native.diagnostics.timing.visual_quality_decode_memory_fallback_count;
  result.visual_quality_gpu_fallback_count = native.diagnostics.timing.visual_quality_gpu_fallback_count;
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

void copy_settings_diagnostics(const NativeEncodeSettings& settings, EncodeResult& result);

void populate_avif_passthrough_diagnostics(const ImageBuffer& image,
                                           AvifEncoderMode requested_encoder,
                                           EncodeResult& result) {
  NativeEncodeSettings settings{};
  settings.requested_avif_encoder = requested_encoder;
  settings.requested_chroma_mode = ChromaMode::auto_keep;
  settings.chroma_mode = native_backend_detail::lossless_source_chroma(image);
  settings.requested_alpha_policy = AlphaModePolicy::automatic;
  settings.requested_bit_depth = source_bit_depth(image);
  settings.bit_depth = settings.requested_bit_depth;
  populate_source_diagnostics(settings, image, requested_encoder);
  if (image.source_info && image.source_info->color_metadata_source == "source-icc") {
    settings.source_has_icc = true;
  }
  settings.avif_encoder = AvifEncoderMode::aom;
  settings.applied_icc = settings.source_has_icc ? "kept" : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "kept" : "none";
  settings.applied_color_primaries = settings.source_color_primaries;
  settings.applied_transfer_characteristics = settings.source_transfer_characteristics;
  settings.applied_matrix_coefficients = settings.source_matrix_coefficients;
  settings.applied_color_range = settings.source_color_range;
  settings.chroma_reason = "无损直通复制 AVIF 码流 chroma";
  settings.bit_depth_reason = "无损直通复制 AVIF 码流 bit-depth";
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.encoder_supports_alpha = true;
  settings.applied_alpha = settings.source_has_alpha_channel ? "kept" : "none";
  settings.alpha_reason = "无损直通复制 AVIF 码流 alpha";
  settings.color_metadata_source = settings.source_has_icc
                                       ? "source-icc"
                                       : (settings.color_metadata_source.empty()
                                              ? "avif-passthrough"
                                              : settings.color_metadata_source);
  settings.color_reason = "无损直通复制 AVIF 码流元数据";
  copy_settings_diagnostics(settings, result);
  result.encoder_id = "avif-passthrough";
  result.requested_encoder_id = avif_encoder_mode_name(requested_encoder);
  result.requested_chroma = chroma_mode_name(ChromaMode::auto_keep);
  result.applied_chroma = settings.chroma_mode == ChromaMode::auto_keep
                              ? "unknown"
                              : chroma_mode_name(settings.chroma_mode);
}

void copy_settings_diagnostics(const NativeEncodeSettings& settings, EncodeResult& result) {
  const auto diagnostics = diagnostics_from_settings(settings);
  result.encoder_id = avif_encoder_mode_name(settings.avif_encoder);
  result.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  result.user_encoder_id = diagnostics.user_encoder_id;
  result.user_chroma = diagnostics.user_chroma;
  result.source_chroma = diagnostics.source_chroma;
  result.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  result.applied_chroma = chroma_mode_name(settings.chroma_mode);
  result.chroma_reason = diagnostics.chroma_reason;
  result.source_bit_depth = diagnostics.source_bit_depth;
  result.requested_bit_depth = settings.requested_bit_depth;
  result.applied_bit_depth = settings.bit_depth;
  result.bit_depth_reason = settings.bit_depth_reason;
  result.alpha_policy = diagnostics.alpha_policy;
  result.source_has_alpha_channel = diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = diagnostics.encoder_supports_alpha;
  result.applied_alpha = diagnostics.applied_alpha;
  result.alpha_reason = diagnostics.alpha_reason;
  result.source_color_primaries = diagnostics.source_color_primaries;
  result.source_transfer_characteristics = diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = diagnostics.source_matrix_coefficients;
  result.source_color_range = diagnostics.source_color_range;
  result.applied_color_primaries = diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = diagnostics.applied_matrix_coefficients;
  result.applied_color_range = diagnostics.applied_color_range;
  result.source_has_icc = diagnostics.source_has_icc;
  result.applied_icc = diagnostics.applied_icc;
  result.source_has_hdr_metadata = diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = diagnostics.applied_hdr_metadata;
  result.color_metadata_source = diagnostics.color_metadata_source;
  result.color_reason = diagnostics.color_reason;
  result.fallback_reason = settings.encoder_fallback_reason;
  result.visual_quality_gpu_requested = diagnostics.visual_quality_gpu_requested;
  result.visual_quality_gpu_used = diagnostics.visual_quality_gpu_used;
  result.visual_quality_gpu_path = diagnostics.visual_quality_gpu_path;
  result.visual_quality_gpu_fallback_reason = diagnostics.visual_quality_gpu_fallback_reason;
  result.speed = settings.speed;
  result.encoder_threads = settings.resources.encoder_threads_per_file;
  result.memory_budget_bytes = settings.resources.memory_limit_bytes;
}

}  // namespace native_backend_detail

export class NativeBackend final {
  struct EncodeOverrides {
    std::optional<AvifEncoderMode> avif_encoder{};
    std::optional<GridPlan> avif_grid_plan{};
  };

 public:
  NativeBackend(const AppConfig& cfg, FileLogger& logger, ResourcePlan resources)
      : cfg_{cfg}, logger_{logger}, resources_{resources} {}

  EncodeResult encode(const ImageFile& image,
                      std::stop_token stop_token = {}) const {
    return encode_with_overrides(image, EncodeOverrides{}, stop_token);
  }

  EncodeResult encode_avif_grid(const ImageFile& image,
                                GridPlan plan,
                                std::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::aom;
    overrides.avif_grid_plan = std::move(plan);
    return encode_with_overrides(image, std::move(overrides), stop_token);
  }

  EncodeResult encode_avif_zenrav1e(const ImageFile& image,
                                    std::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::zenrav1e;
    return encode_with_overrides(image, std::move(overrides), stop_token);
  }

 private:
  using EncodeStartedAt = std::chrono::steady_clock::time_point;

  struct DecodedInput {
    ImageDecodeResult decoded{};
    bool decoder_used_fallback{};
  };

  struct PreparedEncoding {
    NativeEncodeSettings settings{};
    std::unique_ptr<ImageEncoder> encoder{};
    std::string avif_bit_depth_reason{};
  };

  struct PrepareError {
    std::string message{};
    NativeEncodeSettings settings{};
  };

  static std::unexpected<PrepareError> prepare_failed(std::string message,
                                                       const NativeEncodeSettings& settings) {
    return std::unexpected{PrepareError{.message = std::move(message), .settings = settings}};
  }

  [[nodiscard]] EncodeResult initialize_result(const ImageFile& image) const {
    const auto planned_output_path = output_path_for(cfg_, image);
    auto output_path = image.output_path_resolved
                           ? std::expected<fs::path, std::string>{planned_output_path}
                           : resolve_collision_output_path(planned_output_path, cfg_.collision_mode);
    auto result = EncodeResult{.index = image.index,
                               .input_path = image.path,
                               .output_path = output_path ? *output_path : planned_output_path,
                               .original_bytes = image.bytes,
                               .quality = cfg_.quality,
                               .requested_visual_quality = cfg_.visual_quality,
                               .gmsd_weight = GMSD_WEIGHT,
                               .msssim_weight = MSSSIM_WEIGHT,
                               .final_encoder_quality = cfg_.quality,
                               .speed = cfg_.speed.value_or(default_speed_for(cfg_.output_format)),
                               .quality_overridden_by_visual_quality = cfg_.visual_quality.has_value()};
    if (!output_path) {
      mark_failed(result, output_path.error());
    }
    return result;
  }

  static EncodeResult failed_encode_result(const ImageFile& image, std::string_view message) {
    return EncodeResult{.index = image.index,
                        .input_path = image.path,
                        .original_bytes = image.bytes,
                        .processed = true,
                        .ok = false,
                        .message = std::string{message}};
  }

  static void mark_failed(EncodeResult& result, std::string message) {
    result.processed = true;
    result.message = std::move(message);
  }

  static bool cancel_if_requested(EncodeResult& result, std::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
      return false;
    }
    result.canceled = true;
    result.processed = true;
    result.message = "任务已取消。";
    return true;
  }

  [[nodiscard]] std::optional<EncodeResult> try_avif_lossless_passthrough(
      const ImageFile& image,
      const EncodeResult& base_result,
      EncodeStartedAt started,
      std::stop_token stop_token) const {
    if (!native_backend_detail::avif_lossless_passthrough_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto result = base_result;
    auto avif_decoder = AvifImageDecoder{};
    auto container_info = avif_decoder.parse_container_info(image.path);
    if (!container_info) {
      mark_failed(result, redact_path_for_user(container_info.error(), image.path));
      return result;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }
    const auto write_started = native_backend_detail::Clock::now();
    auto copied = native_backend_detail::copy_file_to_output(
        image.path, result.output_path, cfg_.collision_mode == CollisionMode::overwrite,
        stop_token);
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
    if (!copied) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, redact_path_for_user(copied.error(), image.path));
      }
      return result;
    }

    result.output_bytes = *copied;
    result.final_encoder_quality = 100;
    result.search_attempt_count = 1;
    result.lossless = true;
    native_backend_detail::populate_avif_passthrough_diagnostics(*container_info,
                                                                 cfg_.avif_encoder,
                                                                 result);
    result.decoder_id = "libavif-parse";
    result.encoder_id = "avif-passthrough";
    result.integration_mode = "avif-lossless-passthrough";
    result.encoder_threads = 1;
    result.memory_budget_bytes = resources_.memory_limit_bytes;
    result.command = "native:avif-passthrough lossless";
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format("native avif lossless passthrough ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] std::optional<EncodeResult> try_jxl_jpeg_bitstream_transcode(
      const ImageFile& image,
      const EncodeResult& base_result,
      EncodeStartedAt started,
      std::stop_token stop_token) const {
    if (!native_backend_detail::jxl_jpeg_bitstream_transcode_allowed(cfg_, image.path)) {
      return std::nullopt;
    }

    auto result = base_result;
    result.decoder_id = "jpeg-bitstream";
    result.encoder_id = "libjxl";
    result.integration_mode = "jxl-jpeg-bitstream-transcode";
    result.final_encoder_quality = 100;
    result.lossless = true;
    result.encoder_threads = resources_.encoder_threads_per_file;
    result.memory_budget_bytes = resources_.memory_limit_bytes;

    auto settings = native_backend_detail::settings_from_config(cfg_, resources_);
    settings.jxl_jpeg_lossless_candidate = true;

    auto bytes = decoder_common::read_file_bytes(image.path, "JPEG");
    if (!bytes) {
      mark_failed(result, redact_path_for_user(bytes.error(), image.path));
      return result;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    native_backend_detail::populate_jxl_jpeg_bitstream_source_diagnostics(
        settings, std::span<const std::byte>{*bytes}, cfg_);

    auto encoder = JXLImageEncoder{};
    const auto encode_started = native_backend_detail::Clock::now();
    auto encoded = encoder.encode_jpeg_bitstream(std::span<const std::byte>{*bytes},
                                                 settings, stop_token);
    result.encode_seconds = native_backend_detail::elapsed_seconds(encode_started);
    if (!encoded) {
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, encoded.error());
      }
      return result;
    }

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    const auto write_started = native_backend_detail::Clock::now();
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded->encoded.bytes},
            cfg_.collision_mode == CollisionMode::overwrite, stop_token);
        !written) {
      result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return result;
    }
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);

    encoded->diagnostics.timing.encode_seconds = result.encode_seconds;
    encoded->diagnostics.timing.write_seconds = result.write_seconds;
    native_backend_detail::copy_native_result(*encoded, result);
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format("native jpeg bitstream transcode ok: item={:04}",
                         result.index + 1);
    });
    return result;
  }

  [[nodiscard]] std::expected<DecodedInput, std::string> decode_input(
      const ImageFile& image) const {
    auto decoded = decode_image_for_path(
        image.path,
        DecoderRegistryOptions{.allow_wic_fallback = cfg_.allow_wic_fallback,
                               .decode_threads = resources_.encoder_threads_per_file});
    if (!decoded) {
      return std::unexpected{redact_path_for_user(decoded.error(), image.path)};
    }

    const bool decoder_used_fallback = decoded->used_fallback;
    return DecodedInput{.decoded = std::move(*decoded),
                        .decoder_used_fallback = decoder_used_fallback};
  }

  [[nodiscard]] std::expected<PreparedEncoding, PrepareError> prepare_encoding(
      const ImageFile& image,
      const ImageDecodeResult& decoded,
      EncodeOverrides overrides) const {
    PreparedEncoding prepared{};
    prepared.settings = native_backend_detail::settings_from_config(cfg_, resources_);
    const auto requested_avif_encoder = overrides.avif_encoder.value_or(cfg_.avif_encoder);
    prepared.settings.requested_avif_encoder = requested_avif_encoder;
    prepared.settings.requested_chroma_mode = cfg_.chroma_mode;
    prepared.settings.requested_alpha_policy = cfg_.alpha_policy;
    prepared.settings.requested_bit_depth = cfg_.bit_depth;
    if (overrides.avif_grid_plan) {
      prepared.settings.avif_grid_plan = std::move(overrides.avif_grid_plan);
    }
    if (cfg_.output_format == OutputFormat::jxl) {
      prepared.settings.jxl_jpeg_lossless_candidate =
          native_backend_detail::jxl_jpeg_bitstream_transcode_allowed(cfg_, image.path);
    }

    if (cfg_.output_format == OutputFormat::avif) {
      native_backend_detail::populate_source_diagnostics(prepared.settings,
                                                         decoded.image,
                                                         requested_avif_encoder);
      native_backend_detail::populate_color_decision(prepared.settings, cfg_);
      if (decoded.image.width == 0 || decoded.image.height == 0 ||
          decoded.image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
          decoded.image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return prepare_failed("AVIF encoder 输入尺寸超过 API 限制。", prepared.settings);
      }
      if (decoded.image.width > std::numeric_limits<std::uint64_t>::max() / decoded.image.height) {
        return prepare_failed("AVIF encoder 输入尺寸过大。", prepared.settings);
      }
      const auto pixel_count = static_cast<std::uint64_t>(decoded.image.width) *
                               static_cast<std::uint64_t>(decoded.image.height);
      const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(decoded.image,
                                                                             "AVIF encoder");
      if (!has_non_opaque_alpha) {
        return prepare_failed(has_non_opaque_alpha.error(), prepared.settings);
      }
      prepared.settings.has_non_opaque_alpha = *has_non_opaque_alpha;

      const bool avif_lossless = native_backend_detail::avif_lossless_requested(cfg_);
      const auto source_chroma = native_backend_detail::lossless_source_chroma(decoded.image);
      const bool explicit_svt = requested_avif_encoder == AvifEncoderMode::svt;

      const auto selection_requested_encoder =
          avif_lossless && requested_avif_encoder == AvifEncoderMode::automatic
              ? AvifEncoderMode::aom
              : requested_avif_encoder;
      ChromaMode selection_requested_chroma = ChromaMode::auto_keep;
      if (explicit_svt) {
        selection_requested_chroma = ChromaMode::yuv420;
        if (avif_lossless) {
          prepared.settings.chroma_reason =
              "显式选择 SVT q100 使用非像素级无损/最高质量路径，强制使用 420 chroma";
        } else {
          prepared.settings.chroma_reason = cfg_.chroma_mode == ChromaMode::yuv420
                                               ? "显式选择 SVT 使用 420 chroma"
                                               : "显式选择 SVT 有损编码强制使用 420 chroma";
        }
      } else if (avif_lossless) {
        selection_requested_chroma = source_chroma == ChromaMode::auto_keep
                                         ? ChromaMode::yuv444
                                         : source_chroma;
        prepared.settings.chroma_reason = source_chroma == ChromaMode::auto_keep
                                             ? "无损源图 YUV chroma 未知，使用 yuv444 避免 subsampling"
                                             : "无损模式继承源图 YUV chroma";
      } else if (cfg_.chroma_mode != ChromaMode::auto_keep) {
        selection_requested_chroma = cfg_.chroma_mode;
        prepared.settings.chroma_reason = "用户请求 chroma";
      } else if (requested_avif_encoder == AvifEncoderMode::automatic) {
        selection_requested_chroma = ChromaMode::auto_keep;
        prepared.settings.chroma_reason = source_chroma == ChromaMode::auto_keep
                                             ? "encoder/chroma auto 使用编码器推荐 chroma"
                                             : "encoder/chroma auto 不用源图 chroma 限制 encoder selection";
      } else {
        selection_requested_chroma = source_chroma;
        prepared.settings.chroma_reason = source_chroma == ChromaMode::auto_keep
                                             ? "chroma auto，源图 YUV chroma 未知"
                                             : "chroma auto 继承源图 YUV chroma 用于 encoder selection";
      }

      std::optional<int> selection_requested_bit_depth{};
      if (explicit_svt) {
        if (cfg_.bit_depth) {
          selection_requested_bit_depth = cfg_.bit_depth;
          prepared.settings.bit_depth_reason = "用户明确请求 bit-depth";
        } else if (prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth > 10) {
          selection_requested_bit_depth = 10;
          prepared.settings.bit_depth_reason = std::format(
              "显式选择 SVT 将源图 {}-bit 限制为 SVT 支持的 10-bit 输出",
              *prepared.settings.source_bit_depth);
        } else if (prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth >= 10) {
          selection_requested_bit_depth = prepared.settings.source_bit_depth;
          prepared.settings.bit_depth_reason = std::format(
              "显式选择 SVT 继承源图 {}-bit 输出", *prepared.settings.source_bit_depth);
        } else if (avif_lossless) {
          prepared.settings.bit_depth_reason =
              "显式选择 SVT q100 使用 SVT 支持的 8/10-bit auto 输出，不执行严格无损 bit-depth 继承";
        }
      } else if (avif_lossless) {
        selection_requested_bit_depth = native_backend_detail::lossless_source_bit_depth(decoded.image);
        prepared.settings.bit_depth_reason = native_backend_detail::lossless_uses_decoded_bit_depth(decoded.image)
                                             ? "无损模式使用解码后的 8-bit buffer bit-depth"
                                             : "无损模式继承源图 bit-depth";
      } else if (cfg_.bit_depth) {
        selection_requested_bit_depth = cfg_.bit_depth;
        prepared.settings.bit_depth_reason = "用户明确请求 bit-depth";
      } else if (prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth >= 10) {
        selection_requested_bit_depth = prepared.settings.source_bit_depth;
        prepared.settings.bit_depth_reason = std::format(
            "lossy preserved source {}-bit depth", *prepared.settings.source_bit_depth);
      }
      if (!avif_lossless && !selection_requested_bit_depth &&
          prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth == 8) {
        prepared.settings.bit_depth_reason = "有损 auto 可能将 8-bit 源图升至编码器首选 bit-depth";
      }
      if (avif_lossless && !explicit_svt && selection_requested_bit_depth &&
          !native_backend_detail::avif_lossless_bit_depth_supported(*selection_requested_bit_depth)) {
        return prepare_failed(std::format(
                                  "AVIF 无损模式无法保持源图 {}-bit 位深；libavif AOM 当前仅支持 8、10、12-bit 输出。",
                                  *selection_requested_bit_depth),
                              prepared.settings);
      }

      const bool must_preserve_alpha = native_backend_detail::alpha_must_be_preserved(
          cfg_.alpha_policy, prepared.settings.source_has_alpha_channel, *has_non_opaque_alpha);
      const auto selection = select_avif_encoder_for_current_build(AvifEncoderSelectionRequest{
          .requested_encoder = selection_requested_encoder,
          .requested_chroma = selection_requested_chroma,
          .requested_bit_depth = selection_requested_bit_depth,
          .requested_bit_depth_reason = prepared.settings.bit_depth_reason,
          .has_alpha = prepared.settings.source_has_alpha_channel,
          .must_preserve_alpha = must_preserve_alpha,
          .visual_quality_search = cfg_.visual_quality.has_value(),
          .speed_explicit = cfg_.speed.has_value(),
          .allow_zenrav1e_alpha = false,
          .pixel_count = pixel_count,
          .width = static_cast<std::uint32_t>(decoded.image.width),
          .height = static_cast<std::uint32_t>(decoded.image.height),
          .speed = prepared.settings.speed},
          cfg_.enable_experimental_encoders);
      if (!selection) {
        prepared.settings.requested_avif_encoder = selection_requested_encoder;
        prepared.settings.requested_chroma_mode = selection_requested_chroma;
        prepared.settings.requested_bit_depth = selection_requested_bit_depth;
        prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
            selection_requested_encoder);
        prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
            prepared.settings.source_has_alpha_channel, false, prepared.settings.encoder_supports_alpha);
        if (cfg_.alpha_policy == AlphaModePolicy::force &&
            prepared.settings.source_has_alpha_channel && !prepared.settings.encoder_supports_alpha) {
          prepared.settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
        }
        return prepare_failed(selection.error(), prepared.settings);
      }
      prepared.settings.avif_encoder = selection->applied_encoder;
      prepared.settings.chroma_mode = selection->applied_chroma;
      prepared.settings.bit_depth = selection->applied_bit_depth;
      prepared.settings.speed = selection->speed;
      prepared.settings.requested_avif_encoder = selection->requested_encoder;
      prepared.settings.requested_chroma_mode = selection->requested_chroma;
      prepared.settings.requested_bit_depth = selection->requested_bit_depth;
      if (prepared.settings.bit_depth_reason.empty()) {
        prepared.settings.bit_depth_reason = selection->bit_depth_reason;
      }
      prepared.settings.encoder_fallback_reason = selection->fallback_reason;
      prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
          selection->applied_encoder);
      const bool preserve_alpha = prepared.settings.source_has_alpha_channel &&
                                  ((cfg_.alpha_policy == AlphaModePolicy::force &&
                                    prepared.settings.encoder_supports_alpha) ||
                                   (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                                    *has_non_opaque_alpha &&
                                    prepared.settings.encoder_supports_alpha));
      prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
          prepared.settings.source_has_alpha_channel, preserve_alpha,
          prepared.settings.encoder_supports_alpha);
      if (!prepared.settings.source_has_alpha_channel) {
        prepared.settings.alpha_reason = "源图没有 alpha 通道";
      } else if (cfg_.alpha_policy == AlphaModePolicy::off) {
        prepared.settings.alpha_reason = "用户请求移除 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic && !*has_non_opaque_alpha) {
        prepared.settings.alpha_reason = "auto 移除全不透明 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                 prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "auto 保留非不透明 alpha，因为当前编码器支持 alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic) {
        prepared.settings.alpha_reason = "auto 移除 alpha，因为当前编码器不支持 alpha";
      } else if (prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "force 保留源图 alpha 通道";
      } else {
        prepared.settings.alpha_reason = "force 请求保留 alpha，但当前编码器不支持 alpha";
      }
      native_backend_detail::populate_applied_avif_color_diagnostics(prepared.settings,
                                                                     cfg_,
                                                                     selection->applied_encoder,
                                                                     selection->applied_chroma,
                                                                     avif_lossless);
      prepared.avif_bit_depth_reason = prepared.settings.bit_depth_reason;
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return std::format(
            "AVIF decision: item={:04} user_encoder={} user_chroma={} source_chroma={} source_bit_depth={} alpha_policy={} source_alpha={} non_opaque_alpha={} selected_encoder={} requested_chroma={} applied_chroma={} requested_bit_depth={} applied_bit_depth={} applied_alpha={} fallback={} chroma_reason={} alpha_reason={} bit_depth_reason={} color_source={}",
            image.index + 1,
            prepared.settings.user_encoder_id,
            prepared.settings.user_chroma,
            prepared.settings.source_chroma,
            prepared.settings.source_bit_depth ? std::format("{}", *prepared.settings.source_bit_depth) : std::string{""},
            prepared.settings.alpha_policy_name,
            prepared.settings.source_alpha_mode,
            *has_non_opaque_alpha ? "true" : "false",
            avif_encoder_mode_name(selection->applied_encoder),
            chroma_mode_name(selection->requested_chroma),
            chroma_mode_name(selection->applied_chroma),
            selection->requested_bit_depth ? std::format("{}", *selection->requested_bit_depth) : std::string{""},
            selection->applied_bit_depth ? std::format("{}", *selection->applied_bit_depth) : std::string{""},
            prepared.settings.applied_alpha,
            selection->fallback_reason,
            prepared.settings.chroma_reason,
            prepared.settings.alpha_reason,
            prepared.settings.bit_depth_reason,
            prepared.settings.color_metadata_source);
      });
      if (selection->applied_encoder != AvifEncoderMode::svt) {
        prepared.encoder = native_backend_detail::encoder_for_output_format(
            cfg_.output_format, selection->applied_encoder);
      }
    } else {
      if (cfg_.output_format == OutputFormat::jxl || cfg_.output_format == OutputFormat::webp) {
        native_backend_detail::populate_source_image_diagnostics(prepared.settings,
                                                                 decoded.image);
        const auto alpha_decision = native_backend_detail::populate_regular_alpha_decision(
            prepared.settings, decoded.image, cfg_);
        if (!alpha_decision) {
          return prepare_failed(alpha_decision.error(), prepared.settings);
        }
        native_backend_detail::populate_regular_color_decision(prepared.settings, cfg_);
      }
      prepared.encoder = native_backend_detail::encoder_for_output_format(
          cfg_.output_format, requested_avif_encoder);
    }

    if (!prepared.encoder && !(cfg_.output_format == OutputFormat::avif &&
                               prepared.settings.avif_encoder == AvifEncoderMode::svt)) {
      return prepare_failed(std::format("native backend 暂不支持输出格式: {}",
                                        output_format_name(cfg_.output_format)),
                            prepared.settings);
    }
    return std::move(prepared);
  }

  [[nodiscard]] std::expected<NativeEncodeResult, std::string> execute_encode(
      const ImageDecodeResult& decoded,
      ImageEncoder* encoder,
      const NativeEncodeSettings& settings,
      const fs::path& output_path,
      std::stop_token stop_token) const {
    const auto encode_started = native_backend_detail::Clock::now();
    if (stop_token.stop_requested()) {
      return std::unexpected{"任务已取消。"};
    }
    const bool use_svtav1hdr = cfg_.output_format == OutputFormat::avif &&
                               settings.avif_encoder == AvifEncoderMode::svt;
    if (cfg_.visual_quality) {
      auto output_decoder = native_backend_detail::decoder_for_output_format(
          cfg_.output_format, settings.resources.encoder_threads_per_file);
      const auto candidate_path = output_path.parent_path() /
                                  (output_path.filename().wstring() + L".candidate");
      if (use_svtav1hdr) {
        class SvtAv1HdrImageEncoder final : public ImageEncoder {
         public:
          [[nodiscard]] std::string_view id() const noexcept override { return "svt-av1-hdr"; }
          [[nodiscard]] CodecCapabilities capabilities() const override {
            return CodecCapabilities{.output_format = OutputFormat::avif,
                                     .features = CodecFeature::thread_control |
                                                 CodecFeature::visual_quality_search,
                                     .bit_depths = {8, 10}};
          }
          std::expected<NativeEncodeResult, std::string> encode(
              const ImageBuffer& image,
              const NativeEncodeSettings& settings,
              std::stop_token stop_token = {}) const override {
            return encode_svtav1hdr_in_process(image, settings, stop_token);
          }
        } svt_encoder;
        auto search = encode_with_native_visual_quality_search(decoded.image, svt_encoder,
                                                               *output_decoder, settings,
                                                               candidate_path, stop_token);
        if (!search) {
          return std::unexpected{search.error()};
        }
        search->encode_result.diagnostics.timing.encode_seconds =
            search->encode_result.diagnostics.timing.visual_quality_candidate_encode_seconds;
        search->encode_result.visual_quality_target_met = search->target_met;
        return std::move(search->encode_result);
      }

      auto search = encode_with_native_visual_quality_search(decoded.image, *encoder,
                                                             *output_decoder, settings,
                                                             candidate_path, stop_token);
      if (!search) {
        return std::unexpected{search.error()};
      }
      search->encode_result.diagnostics.timing.encode_seconds =
          search->encode_result.diagnostics.timing.visual_quality_candidate_encode_seconds;
      search->encode_result.visual_quality_target_met = search->target_met;
      return std::move(search->encode_result);
    }

    if (use_svtav1hdr) {
      auto encoded = encode_svtav1hdr_in_process(decoded.image, settings, stop_token);
      if (encoded) {
        encoded->diagnostics.timing.encode_seconds =
            native_backend_detail::elapsed_seconds(encode_started);
      }
      return encoded;
    }
    auto encoded = encoder->encode(decoded.image, settings, stop_token);
    if (encoded) {
      encoded->diagnostics.timing.encode_seconds =
          native_backend_detail::elapsed_seconds(encode_started);
    }
    return encoded;
  }

  [[nodiscard]] EncodeResult finalize_result(EncodeResult result,
                                             NativeEncodeResult encoded,
                                             const ImageDecodeResult& decoded,
                                             bool decoder_used_fallback,
                                             std::string avif_bit_depth_reason,
                                             EncodeStartedAt started,
                                             std::stop_token stop_token) const {
    const auto write_started = native_backend_detail::Clock::now();
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded.encoded.bytes},
            cfg_.collision_mode == CollisionMode::overwrite, stop_token);
        !written) {
      result.write_seconds = native_backend_detail::elapsed_seconds(write_started);
      if (stop_token.stop_requested()) {
        cancel_if_requested(result, stop_token);
      } else {
        mark_failed(result, written.error());
      }
      return result;
    }
    result.write_seconds = native_backend_detail::elapsed_seconds(write_started);

    encoded.diagnostics.decoder_id = decoded.decoder_id;
    encoded.diagnostics.used_decoder_fallback = decoder_used_fallback;
    native_backend_detail::merge_stage_timing(encoded, result);
    native_backend_detail::copy_native_result(encoded, result);
    if (!decoded.decoder_id.empty()) {
      result.decoder_id = decoded.decoder_id;
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
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format("native encode ok: item={:04}", result.index + 1);
    });
    native_backend_detail::log_info_noexcept(logger_, [&] {
      return std::format(
          "native timing: item={:04} total={}s decode={}s prepare={}s encode={}s vq={}s metrics={}s write={}s",
          result.index + 1,
          native_backend_detail::format_timing_seconds(result.seconds),
          native_backend_detail::format_timing_seconds(result.decode_seconds),
          native_backend_detail::format_timing_seconds(result.prepare_seconds),
          native_backend_detail::format_timing_seconds(result.encode_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_search_seconds),
          native_backend_detail::format_timing_seconds(result.visual_quality_metric_seconds),
          native_backend_detail::format_timing_seconds(result.write_seconds));
    });
    if (result.visual_quality_search_seconds >= 0.0) {
      native_backend_detail::log_info_noexcept(logger_, [&] {
        return std::format(
            "native vq breakdown: item={:04} candidates={} memory_fallback_decodes={} gpu_requested={} gpu_used={} gpu_path={} gpu_fallbacks={} gpu_fallback_reason={} candidate_encode={}s candidate_decode={}s candidate_io={}s luma={}s gmsd={}s ms_ssim={}s metrics={}s metric_share={}%",
            result.index + 1,
            result.visual_quality_candidate_count,
            result.visual_quality_decode_memory_fallback_count,
            result.visual_quality_gpu_requested ? "true" : "false",
            result.visual_quality_gpu_used ? "true" : "false",
            result.visual_quality_gpu_path.empty() ? "-" : result.visual_quality_gpu_path,
            result.visual_quality_gpu_fallback_count,
            result.visual_quality_gpu_fallback_reason.empty()
                ? "-"
                : result.visual_quality_gpu_fallback_reason,
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_encode_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_decode_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_candidate_io_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_luma_seconds),
            native_backend_detail::format_timing_seconds(result.gmsd_seconds),
            native_backend_detail::format_timing_seconds(result.ms_ssim_seconds),
            native_backend_detail::format_timing_seconds(result.visual_quality_metric_seconds),
            native_backend_detail::format_timing_share(result.visual_quality_metric_seconds,
                                                      result.visual_quality_search_seconds));
      });
    }
    return result;
  }

  EncodeResult encode_with_overrides(const ImageFile& image,
                                     EncodeOverrides overrides,
                                     std::stop_token stop_token = {}) const {
    try {
      const auto started = std::chrono::steady_clock::now();
      auto result = initialize_result(image);

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }
      if (result.processed && !result.ok) {
        return result;
      }

      if (cfg_.collision_mode == CollisionMode::skip) {
        std::error_code exists_ec;
        const auto output_exists = fs::exists(result.output_path, exists_ec);
        if (exists_ec) {
          result.processed = true;
          result.ok = false;
          result.message = std::format("无法检查输出路径 {}: {}",
                                       display_path_for_user(result.output_path), exists_ec.message());
          return result;
        }
        if (output_exists) {
          result.processed = true;
          result.ok = true;
          result.skipped = true;
          result.message = "输出已存在，已跳过。";
          return result;
        }
      }

      if (auto passthrough = try_avif_lossless_passthrough(image, result, started, stop_token)) {
        return std::move(*passthrough);
      }

      if (auto transcode = try_jxl_jpeg_bitstream_transcode(image, result, started, stop_token)) {
        return std::move(*transcode);
      }

      const auto decode_started = native_backend_detail::Clock::now();
      auto decoded_input = decode_input(image);
      result.decode_seconds = native_backend_detail::elapsed_seconds(decode_started);
      if (!decoded_input) {
        mark_failed(result, decoded_input.error());
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      const auto prepare_started = native_backend_detail::Clock::now();
      auto prepared = prepare_encoding(image, decoded_input->decoded, std::move(overrides));
      result.prepare_seconds = native_backend_detail::elapsed_seconds(prepare_started);
      if (!prepared) {
        native_backend_detail::copy_settings_diagnostics(prepared.error().settings, result);
        mark_failed(result, prepared.error().message);
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      const auto encode_started = native_backend_detail::Clock::now();
      auto encoded = execute_encode(decoded_input->decoded, prepared->encoder.get(),
                                    prepared->settings, result.output_path, stop_token);
      result.encode_seconds = native_backend_detail::elapsed_seconds(encode_started);
      if (!encoded) {
        native_backend_detail::copy_settings_diagnostics(prepared->settings, result);
        if (stop_token.stop_requested()) {
          cancel_if_requested(result, stop_token);
        } else {
          mark_failed(result, encoded.error());
        }
        return result;
      }

      if (cancel_if_requested(result, stop_token)) {
        return result;
      }

      return finalize_result(std::move(result), std::move(*encoded), decoded_input->decoded,
                             decoded_input->decoder_used_fallback,
                             std::move(prepared->avif_bit_depth_reason), started, stop_token);
    } catch (const std::bad_alloc&) {
      return failed_encode_result(image, "native backend 单项转换内存不足。");
    } catch (const std::length_error&) {
      return failed_encode_result(image, "native backend 单项转换数据超过运行时限制。");
    } catch (const std::filesystem::filesystem_error&) {
      return failed_encode_result(image, "native backend 单项转换文件系统访问失败。");
    }
  }

 private:
  const AppConfig& cfg_;
  FileLogger& logger_;
  ResourcePlan resources_;
};

}  // namespace awj
