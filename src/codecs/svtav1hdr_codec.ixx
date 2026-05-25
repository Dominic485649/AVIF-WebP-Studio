module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

#include <avif/avif.h>

#ifndef AWJ_HAS_SVTAV1HDR_STATIC
#define AWJ_HAS_SVTAV1HDR_STATIC 0
#endif

export module awj.svtav1hdr_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.image;

export namespace awj {

namespace svtav1hdr_detail {

namespace fs = std::filesystem;

struct AvifImageDeleter {
  void operator()(avifImage* value) const noexcept {
    if (value != nullptr) {
      avifImageDestroy(value);
    }
  }
};

struct AvifEncoderDeleter {
  void operator()(avifEncoder* value) const noexcept {
    if (value != nullptr) {
      avifEncoderDestroy(value);
    }
  }
};

struct AvifRwDataDeleter {
  void operator()(avifRWData* value) const noexcept {
    if (value != nullptr) {
      avifRWDataFree(value);
      delete value;
    }
  }
};

struct AvifRgbPixels {
  explicit AvifRgbPixels(avifRGBImage* value) : rgb{value} {}
  avifRGBImage* rgb{};
  ~AvifRgbPixels() {
    if (rgb != nullptr) {
      avifRGBImageFreePixels(rgb);
    }
  }
  AvifRgbPixels(const AvifRgbPixels&) = delete;
  AvifRgbPixels& operator=(const AvifRgbPixels&) = delete;
};

using AvifImage = std::unique_ptr<avifImage, AvifImageDeleter>;
using AvifEncoder = std::unique_ptr<avifEncoder, AvifEncoderDeleter>;
using AvifRwData = std::unique_ptr<avifRWData, AvifRwDataDeleter>;

struct RgbaPlaneView {
  const ImagePlane* plane{};
};

std::expected<RgbaPlaneView, std::string> rgba8_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 || image.planes.empty()) {
    return std::unexpected{"svt-av1-hdr 当前需要 8-bit RGBA ImageBuffer。"};
  }
  if (image.width == 0 || image.height == 0) {
    return std::unexpected{"svt-av1-hdr 输入图片尺寸为空。"};
  }
  const auto& plane = image.planes.front();
  if (image.width > std::numeric_limits<std::size_t>::max() / 4) {
    return std::unexpected{"svt-av1-hdr 输入宽度过大。"};
  }
  const std::size_t min_stride = image.width * 4;
  if (plane.stride < min_stride) {
    return std::unexpected{"svt-av1-hdr 输入 RGBA stride 无效。"};
  }
  if (image.height > 1 && plane.stride > (std::numeric_limits<std::size_t>::max() - min_stride) / (image.height - 1)) {
    return std::unexpected{"svt-av1-hdr 输入 buffer 过大。"};
  }
  const std::size_t required = (image.height - 1) * plane.stride + min_stride;
  if (plane.bytes.size() < required) {
    return std::unexpected{"svt-av1-hdr 输入 RGBA buffer 不完整。"};
  }
  return RgbaPlaneView{.plane = &plane};
}

std::expected<std::size_t, std::string> checked_interleaved_stride(std::size_t width,
                                                                 std::size_t channels,
                                                                 std::size_t bytes_per_sample,
                                                                 std::string_view context) {
  if (channels == 0 || bytes_per_sample == 0 ||
      width > std::numeric_limits<std::size_t>::max() / channels / bytes_per_sample) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  const auto stride = width * channels * bytes_per_sample;
  if (stride > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{std::format("{} 输入 stride 超出 libavif 限制。", context)};
  }
  return stride;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  return stride * height;
}

std::expected<std::size_t, std::string> checked_pixel_count(std::size_t width,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / height) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  return width * height;
}

std::uint16_t expand_u8_to_depth(std::uint8_t value, int bit_depth) noexcept {
  const auto max_value = static_cast<std::uint32_t>((1u << bit_depth) - 1u);
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * max_value + 127u) / 255u);
}

std::expected<avifRange, std::string> avif_range_from_settings(const SvtAv1HdrSettings& settings) {
  if (!settings.color_range) {
    return AVIF_RANGE_FULL;
  }
  switch (*settings.color_range) {
    case 0:
      return AVIF_RANGE_LIMITED;
    case 1:
      return AVIF_RANGE_FULL;
    default:
      return std::unexpected{"svt-av1-hdr color-range 只支持 0(limited) 或 1(full)。"};
  }
}

int avif_quality_from_crf(int crf) noexcept {
  return std::clamp((63 - std::clamp(crf, 0, 63)) * 100 / 63, 0, 100);
}

std::string svt_error_message(std::string_view operation, const avifEncoder& encoder, avifResult result) {
  std::string message = std::format("svt-av1-hdr {} 失败: {}", operation, avifResultToString(result));
  if (encoder.diag.error[0] != '\0') {
    message += ": ";
    message += encoder.diag.error;
  }
  return message;
}

std::expected<AvifImage, std::string> avif_image_from_rgba(const ImageBuffer& image,
                                                           const NativeEncodeSettings& settings,
                                                           int bit_depth) {
  auto plane_view = rgba8_plane(image);
  if (!plane_view) {
    return std::unexpected{plane_view.error()};
  }
  const auto* plane = plane_view->plane;

  AvifImage avif_image{avifImageCreate(static_cast<uint32_t>(image.width),
                                       static_cast<uint32_t>(image.height),
                                       static_cast<uint32_t>(bit_depth),
                                       AVIF_PIXEL_FORMAT_YUV420)};
  if (!avif_image) {
    return std::unexpected{"无法创建 svt-av1-hdr AVIF image。"};
  }
  avif_image->colorPrimaries = static_cast<avifColorPrimaries>(
      settings.svtav1hdr.color_primaries.value_or(AVIF_COLOR_PRIMARIES_BT709));
  avif_image->transferCharacteristics = static_cast<avifTransferCharacteristics>(
      settings.svtav1hdr.transfer_characteristics.value_or(AVIF_TRANSFER_CHARACTERISTICS_SRGB));
  avif_image->matrixCoefficients = static_cast<avifMatrixCoefficients>(
      settings.svtav1hdr.matrix_coefficients.value_or(AVIF_MATRIX_COEFFICIENTS_BT709));
  const auto range = avif_range_from_settings(settings.svtav1hdr);
  if (!range) {
    return std::unexpected{range.error()};
  }
  avif_image->yuvRange = *range;

  avifRGBImage rgb{};
  avifRGBImageSetDefaults(&rgb, avif_image.get());
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.depth = static_cast<std::uint32_t>(bit_depth);
  rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
  if (bit_depth == 8) {
    const auto row_bytes = checked_interleaved_stride(image.width, 3, 1, "svt-av1-hdr");
    if (!row_bytes) {
      return std::unexpected{row_bytes.error()};
    }
    const auto image_bytes = checked_image_bytes(*row_bytes, image.height, "svt-av1-hdr");
    if (!image_bytes) {
      return std::unexpected{image_bytes.error()};
    }
    std::vector<std::uint8_t> rgb_pixels(*image_bytes);
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = reinterpret_cast<const std::uint8_t*>(plane->bytes.data() + y * plane->stride);
      auto* out = rgb_pixels.data() + y * *row_bytes;
      for (std::size_t x = 0; x < image.width; ++x) {
        out[x * 3 + 0] = row[x * 4 + 0];
        out[x * 3 + 1] = row[x * 4 + 1];
        out[x * 3 + 2] = row[x * 4 + 2];
      }
    }
    rgb.pixels = rgb_pixels.data();
    rgb.rowBytes = static_cast<std::uint32_t>(*row_bytes);
    const avifResult converted = avifImageRGBToYUV(avif_image.get(), &rgb);
    if (converted != AVIF_RESULT_OK) {
      return std::unexpected{std::format("svt-av1-hdr RGB 转 YUV 失败: {}", avifResultToString(converted))};
    }
    return avif_image;
  }

  const auto pixel_count = checked_pixel_count(image.width, image.height, "svt-av1-hdr");
  if (!pixel_count) {
    return std::unexpected{pixel_count.error()};
  }
  if (*pixel_count > std::numeric_limits<std::size_t>::max() / 3) {
    return std::unexpected{"svt-av1-hdr 高位深临时 buffer 过大。"};
  }
  const auto row_bytes = checked_interleaved_stride(
      image.width, 3, sizeof(std::uint16_t), "svt-av1-hdr");
  if (!row_bytes) {
    return std::unexpected{row_bytes.error()};
  }
  std::vector<std::uint16_t> high_depth_pixels(*pixel_count * 3);
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(plane->bytes.data() + y * plane->stride);
    auto* out = high_depth_pixels.data() + y * image.width * 3;
    for (std::size_t x = 0; x < image.width; ++x) {
      out[x * 3 + 0] = expand_u8_to_depth(row[x * 4 + 0], bit_depth);
      out[x * 3 + 1] = expand_u8_to_depth(row[x * 4 + 1], bit_depth);
      out[x * 3 + 2] = expand_u8_to_depth(row[x * 4 + 2], bit_depth);
    }
  }
  rgb.pixels = reinterpret_cast<std::uint8_t*>(high_depth_pixels.data());
  rgb.rowBytes = static_cast<std::uint32_t>(*row_bytes);
  const avifResult converted = avifImageRGBToYUV(avif_image.get(), &rgb);
  if (converted != AVIF_RESULT_OK) {
    return std::unexpected{std::format("svt-av1-hdr RGB 转 YUV 失败: {}", avifResultToString(converted))};
  }
  return avif_image;
}

std::expected<std::vector<std::byte>, std::string> encode_avif_with_svt_backend(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    int bit_depth) {
  auto avif_image = avif_image_from_rgba(image, settings, bit_depth);
  if (!avif_image) {
    return std::unexpected{avif_image.error()};
  }

  AvifEncoder encoder{avifEncoderCreate()};
  if (!encoder) {
    return std::unexpected{"无法创建 svt-av1-hdr AVIF encoder。"};
  }
  encoder->codecChoice = AVIF_CODEC_CHOICE_SVT;
  encoder->quality = settings.svtav1hdr.crf ? avif_quality_from_crf(*settings.svtav1hdr.crf)
                                            : std::clamp(settings.quality, 1, 100);
  encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
  encoder->speed = settings.svtav1hdr.preset;
  encoder->maxThreads = std::max(1, settings.resources.encoder_threads_per_file);
  encoder->keyframeInterval = settings.svtav1hdr.keyint;

  const auto set_option = [&](std::string_view key, std::string_view value) -> std::expected<void, std::string> {
    const avifResult result = avifEncoderSetCodecSpecificOption(encoder.get(), std::string{key}.c_str(), std::string{value}.c_str());
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{svt_error_message(std::format("设置参数 {}={}", key, value), *encoder, result)};
    }
    return {};
  };
  if (auto set = set_option("tune", settings.svtav1hdr.tune); !set) {
    return std::unexpected{set.error()};
  }
  if (auto set = set_option("keyint", std::to_string(settings.svtav1hdr.keyint)); !set) {
    return std::unexpected{set.error()};
  }
  if (auto set = set_option("avif", settings.svtav1hdr.avif ? "1" : "0"); !set) {
    return std::unexpected{set.error()};
  }
  for (const auto& param : settings.svtav1hdr.params) {
    const auto text = utf8_from_wide(param);
    const auto equals = text.find('=');
    if (equals == std::string::npos || equals == 0) {
      return std::unexpected{std::format("svt-av1-hdr 参数必须为 key=value: {}", text)};
    }
    if (auto set = set_option(std::string_view{text}.substr(0, equals), std::string_view{text}.substr(equals + 1)); !set) {
      return std::unexpected{set.error()};
    }
  }

  AvifRwData output{new avifRWData{}};
  const avifResult result = avifEncoderWrite(encoder.get(), (*avif_image).get(), output.get());
  if (result != AVIF_RESULT_OK) {
    return std::unexpected{svt_error_message("编码", *encoder, result)};
  }
  if (output->size == 0 || output->data == nullptr) {
    return std::unexpected{"svt-av1-hdr 输出 AVIF 为空。"};
  }
  std::vector<std::byte> bytes(output->size);
  std::memcpy(bytes.data(), output->data, output->size);
  return bytes;
}

std::string hdr_metadata_summary(const SvtAv1HdrSettings& settings) {
  std::string out;
  const auto append = [&](std::string_view key, std::string value) {
    if (!out.empty()) {
      out += ';';
    }
    out += key;
    out += '=';
    out += std::move(value);
  };
  if (settings.color_primaries) {
    append("color-primaries", std::to_string(*settings.color_primaries));
  }
  if (settings.transfer_characteristics) {
    append("transfer-characteristics", std::to_string(*settings.transfer_characteristics));
  }
  if (settings.matrix_coefficients) {
    append("matrix-coefficients", std::to_string(*settings.matrix_coefficients));
  }
  if (settings.color_range) {
    append("color-range", std::to_string(*settings.color_range));
  }
  if (!settings.mastering_display.empty()) {
    append("mastering-display", utf8_from_wide(settings.mastering_display));
  }
  if (!settings.content_light.empty()) {
    append("content-light", utf8_from_wide(settings.content_light));
  }
  return out;
}

}  // namespace svtav1hdr_detail

export bool svtav1hdr_encoder_build_available() noexcept {
#if AWJ_HAS_SVTAV1HDR_STATIC
  return avifCodecName(AVIF_CODEC_CHOICE_SVT, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr;
#else
  return false;
#endif
}

export bool svtav1hdr_static_library_available() noexcept {
#if AWJ_HAS_SVTAV1HDR_STATIC
  return true;
#else
  return false;
#endif
}

export std::expected<NativeEncodeResult, std::string> encode_svtav1hdr_in_process(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings) {
#if !AWJ_HAS_SVTAV1HDR_STATIC
  (void)image;
  (void)settings;
  return std::unexpected{"svt-av1-hdr static backend is not available in this build."};
#else
  if (settings.quality >= 100 || settings.visual_quality == 100) {
    return std::unexpected{
        "svt-av1-hdr 只支持 420 色度采样，不能保证 AVIF 无损模式继承源图参数；请使用 --avif-encoder auto/aom。"};
  }
  if (settings.chroma_mode != ChromaMode::yuv420) {
    return std::unexpected{"svt-av1-hdr only supports 420 chroma."};
  }
  const int bit_depth = settings.bit_depth.value_or(8);
  if (bit_depth != 8 && bit_depth != 10) {
    return std::unexpected{"svt-av1-hdr 当前只支持 8-bit 或 10-bit 输出。"};
  }
  if (!svtav1hdr_encoder_build_available()) {
    return std::unexpected{"svt-av1-hdr static backend was built without libavif SVT encoder support."};
  }

  auto bytes = svtav1hdr_detail::encode_avif_with_svt_backend(image, settings, bit_depth);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }

  const auto hdr_summary = svtav1hdr_detail::hdr_metadata_summary(settings.svtav1hdr);
  auto diagnostics = diagnostics_from_settings(settings);
  diagnostics.encoder_id = "svt-av1-hdr";
  diagnostics.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  diagnostics.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  diagnostics.applied_chroma = chroma_mode_name(ChromaMode::yuv420);
  diagnostics.requested_bit_depth = settings.requested_bit_depth;
  diagnostics.applied_bit_depth = bit_depth;
  diagnostics.bit_depth_reason = settings.bit_depth_reason.empty()
                                     ? (settings.requested_bit_depth ? "explicit bit-depth requested"
                                                                     : "auto selected svt-av1-hdr bit-depth")
                                     : settings.bit_depth_reason;
  diagnostics.fallback_reason = settings.encoder_fallback_reason;
  diagnostics.encoder_license = "BSD-3-Clause";
  diagnostics.integration_mode = "static-svtav1hdr";
  diagnostics.svtav1hdr_crf = settings.svtav1hdr.crf;
  diagnostics.svtav1hdr_preset = settings.svtav1hdr.preset;
  diagnostics.svtav1hdr_tune = settings.svtav1hdr.tune;
  diagnostics.svtav1hdr_keyint = settings.svtav1hdr.keyint;
  diagnostics.svtav1hdr_hdr_metadata = hdr_summary;
  diagnostics.svtav1hdr_note = "svt-av1-hdr statically linked through libavif SVT backend";
  diagnostics.speed_mapping = SpeedMapping{.user_speed = settings.speed,
                                           .codec_value = settings.svtav1hdr.preset,
                                           .codec_key = "svt-av1-hdr:preset"};
  diagnostics.encoder_threads = settings.resources.encoder_threads_per_file;
  diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;

  return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*bytes),
                                                    .codec_name = "svt-av1-hdr"},
                            .diagnostics = std::move(diagnostics),
                            .final_quality = settings.quality,
                            .lossless = false,
                            .search_attempt_count = 1};
#endif
}

}  // namespace awj
