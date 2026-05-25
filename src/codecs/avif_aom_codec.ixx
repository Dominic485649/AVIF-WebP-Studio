module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef AWJ_HAS_ZENRAVIF
#define AWJ_HAS_ZENRAVIF 0
#endif

#include <avif/avif.h>

export module awj.avif_aom_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;
import awj.avif_registry;

#if AWJ_HAS_ZENRAVIF
extern "C" {
struct ZenravifOutput {
  std::uint8_t* data;
  std::size_t size;
};

int zenravif_bridge_encode_rgba8(const std::uint8_t* pixels,
                                 std::size_t width,
                                 std::size_t height,
                                 std::size_t stride,
                                 int quality,
                                 int speed,
                                 int bit_depth,
                                 int chroma,
                                 std::size_t threads,
                                 int keyint,
                                 bool still_picture,
                                 bool enable_qm,
                                 double vaq_strength,
                                 bool enable_trellis,
                                 bool rdo_tx_decision,
                                 ZenravifOutput* out,
                                 std::uint8_t* error_out,
                                 std::size_t error_capacity);
void zenravif_bridge_free(std::uint8_t* data, std::size_t size);
}
#endif

export namespace awj {

namespace avif_aom_detail {

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

struct AvifDecoderDeleter {
  void operator()(avifDecoder* value) const noexcept {
    if (value != nullptr) {
      avifDecoderDestroy(value);
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

#if AWJ_HAS_ZENRAVIF
struct ZenravifBytes {
  ZenravifBytes() = default;
  ZenravifOutput output{};
  ~ZenravifBytes() {
    if (output.data != nullptr) {
      zenravif_bridge_free(output.data, output.size);
    }
  }
  ZenravifBytes(const ZenravifBytes&) = delete;
  ZenravifBytes& operator=(const ZenravifBytes&) = delete;
};
#endif

using AvifImage = std::unique_ptr<avifImage, AvifImageDeleter>;
using AvifEncoder = std::unique_ptr<avifEncoder, AvifEncoderDeleter>;
using AvifDecoder = std::unique_ptr<avifDecoder, AvifDecoderDeleter>;
using AvifRwData = std::unique_ptr<avifRWData, AvifRwDataDeleter>;

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "AVIF");
}

std::expected<std::size_t, std::string> checked_interleaved_stride(std::size_t width,
                                                                 std::size_t channels,
                                                                 std::string_view context,
                                                                 std::size_t bytes_per_sample = 1) {
  if (channels == 0 || bytes_per_sample == 0 ||
      width > std::numeric_limits<std::size_t>::max() / channels / bytes_per_sample) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  return width * channels * bytes_per_sample;
}

std::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context,
                                                            std::size_t bytes_per_sample = 1) {
  return checked_interleaved_stride(width, 4, context, bytes_per_sample);
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

std::expected<const ImagePlane*, std::string> rgba8_plane(const ImageBuffer& image,
                                                          std::string_view encoder_id) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{std::format("{} encoder 当前需要 8-bit RGBA ImageBuffer。", encoder_id)};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = checked_rgba_stride(image.width, encoder_id);
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, encoder_id);
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.stride < *expected_stride || plane.bytes.size() < *expected_bytes) {
    return std::unexpected{std::format("{} encoder 输入 RGBA buffer 尺寸无效。", encoder_id)};
  }
  return &plane;
}

std::expected<avifPixelFormat, std::string> avif_pixel_format_from_chroma(
    ChromaMode chroma) {
  switch (chroma) {
    case ChromaMode::yuv420:
    case ChromaMode::auto_keep:
      return AVIF_PIXEL_FORMAT_YUV420;
    case ChromaMode::yuv422:
      return AVIF_PIXEL_FORMAT_YUV422;
    case ChromaMode::yuv444:
      return AVIF_PIXEL_FORMAT_YUV444;
    default:
      return std::unexpected{"AVIF encoder 色度采样参数无效。"};
  }
}

ChromaMode chroma_from_pixel_format(PixelFormat pixel_format) noexcept {
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

ChromaMode applied_chroma_from_settings(const ImageBuffer& image,
                                        ChromaMode chroma,
                                        bool lossless) noexcept {
  if (chroma != ChromaMode::auto_keep) {
    return chroma;
  }
  if (lossless && image.source_info) {
    const auto source_chroma = chroma_from_pixel_format(image.source_info->pixel_format);
    if (source_chroma != ChromaMode::auto_keep) {
      return source_chroma;
    }
  }
  return ChromaMode::yuv420;
}

std::optional<int> avif_bit_depth_from_source(const ImageBuffer& image) noexcept {
  if (!image.source_info || image.source_info->bit_depth <= 0) {
    return {};
  }
  const int depth = image.source_info->bit_depth;
  if (depth == 8 || depth == 10 || depth == 12) {
    return depth;
  }
  return {};
}

bool preserve_alpha_for_encode(const NativeEncodeSettings& settings) noexcept {
  return settings.source_has_alpha_channel && settings.encoder_supports_alpha &&
         settings.applied_alpha == "kept";
}

std::optional<int> color_value_for_encode(std::optional<int> value, int unspecified) noexcept {
  if (!value || *value == unspecified) {
    return {};
  }
  return value;
}

avifRange range_for_encode(std::optional<int> range) noexcept {
  if (!range) {
    return AVIF_RANGE_FULL;
  }
  return *range == 0 ? AVIF_RANGE_LIMITED : AVIF_RANGE_FULL;
}

avifMatrixCoefficients matrix_coefficients_for_encode(const NativeEncodeSettings& settings,
                                                      ChromaMode chroma,
                                                      bool lossless) noexcept;

void apply_color_settings(avifImage& avif_image,
                          const NativeEncodeSettings& settings,
                          ChromaMode chroma,
                          bool lossless) noexcept {
  avif_image.colorPrimaries = static_cast<avifColorPrimaries>(
      color_value_for_encode(settings.applied_color_primaries,
                             AVIF_COLOR_PRIMARIES_UNSPECIFIED)
          .value_or(AVIF_COLOR_PRIMARIES_BT709));
  avif_image.transferCharacteristics = static_cast<avifTransferCharacteristics>(
      color_value_for_encode(settings.applied_transfer_characteristics,
                             AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED)
          .value_or(AVIF_TRANSFER_CHARACTERISTICS_SRGB));
  avif_image.matrixCoefficients = matrix_coefficients_for_encode(settings, chroma, lossless);
  avif_image.yuvRange = range_for_encode(settings.applied_color_range);
}

std::optional<int> int_from_avif_color(avifColorPrimaries value) noexcept {
  return value == AVIF_COLOR_PRIMARIES_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_transfer(avifTransferCharacteristics value) noexcept {
  return value == AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_matrix(avifMatrixCoefficients value) noexcept {
  return value == AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_range(avifRange value) noexcept {
  switch (value) {
    case AVIF_RANGE_LIMITED:
      return 0;
    case AVIF_RANGE_FULL:
      return 1;
    default:
      return {};
  }
}

bool has_avif_icc(const avifImage& image) noexcept {
  return image.icc.size > 0 && image.icc.data != nullptr;
}

bool has_hdr_cicp(const avifImage& image) noexcept {
  return static_cast<int>(image.colorPrimaries) == 9 ||
         static_cast<int>(image.transferCharacteristics) == 16 ||
         static_cast<int>(image.transferCharacteristics) == 18;
}

std::string color_metadata_source_from_avif(const avifImage& image) {
  if (image.colorPrimaries != AVIF_COLOR_PRIMARIES_UNSPECIFIED ||
      image.transferCharacteristics != AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED ||
      image.matrixCoefficients != AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED) {
    return "source-cicp";
  }
  if (has_avif_icc(image)) {
    return "source-icc";
  }
  return "unknown";
}

int applied_bit_depth_from_settings(const ImageBuffer& image,
                                    const NativeEncodeSettings& settings,
                                    bool lossless) {
  if (settings.bit_depth) {
    return *settings.bit_depth;
  }
  if (lossless) {
    if (const auto source_depth = avif_bit_depth_from_source(image)) {
      return *source_depth;
    }
    return image.bit_depth;
  }
  return 8;
}

std::string default_bit_depth_reason(const ImageBuffer& image,
                                     const NativeEncodeSettings& settings,
                                     bool lossless) {
  if (settings.bit_depth_explicit) {
    return "explicit bit-depth requested";
  }
  if (lossless && avif_bit_depth_from_source(image)) {
    return "lossless inherited source bit-depth";
  }
  if (lossless) {
    return "lossless inherited decoded bit-depth";
  }
  return "auto selected encoder default bit-depth";
}

avifMatrixCoefficients matrix_coefficients_for_encode(const NativeEncodeSettings& settings,
                                                      ChromaMode chroma,
                                                      bool lossless) noexcept {
  if (const auto value = color_value_for_encode(settings.applied_matrix_coefficients,
                                                AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED)) {
    return static_cast<avifMatrixCoefficients>(*value);
  }
  if (lossless && chroma == ChromaMode::yuv444) {
    return AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  }
  return AVIF_MATRIX_COEFFICIENTS_BT709;
}

int chroma_numeric(ChromaMode chroma) noexcept {
  switch (chroma) {
    case ChromaMode::yuv444:
      return 444;
    case ChromaMode::yuv420:
    case ChromaMode::auto_keep:
    default:
      return 420;
  }
}

std::uint16_t expand_u8_to_depth(std::uint8_t value, int bit_depth) noexcept {
  const auto max_value = static_cast<std::uint32_t>((1u << bit_depth) - 1u);
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * max_value + 127u) / 255u);
}

struct RgbSource {
  avifRGBImage rgb{};
  std::vector<std::uint8_t> low_depth_pixels{};
  std::vector<std::uint16_t> high_depth_pixels{};
};

struct Rgba8Source {
  const std::uint8_t* pixels{};
  std::size_t stride{};
  std::vector<std::uint8_t> pixels_without_alpha{};
};

std::expected<Rgba8Source, std::string> rgba8_source_for_bridge(
    const ImageBuffer& image,
    const ImagePlane& plane,
    const NativeEncodeSettings& settings,
    std::string_view context) {
  Rgba8Source source{.pixels = reinterpret_cast<const std::uint8_t*>(plane.bytes.data()),
                     .stride = plane.stride};
  if (preserve_alpha_for_encode(settings)) {
    return source;
  }
  const auto row_bytes = checked_rgba_stride(image.width, context);
  if (!row_bytes) {
    return std::unexpected{row_bytes.error()};
  }
  const auto image_bytes = checked_image_bytes(*row_bytes, image.height, context);
  if (!image_bytes) {
    return std::unexpected{image_bytes.error()};
  }
  source.pixels_without_alpha.resize(*image_bytes);
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(plane.bytes.data() + y * plane.stride);
    auto* out = source.pixels_without_alpha.data() + y * *row_bytes;
    for (std::size_t x = 0; x < image.width; ++x) {
      out[x * 4 + 0] = row[x * 4 + 0];
      out[x * 4 + 1] = row[x * 4 + 1];
      out[x * 4 + 2] = row[x * 4 + 2];
      out[x * 4 + 3] = 255;
    }
  }
  source.pixels = source.pixels_without_alpha.data();
  source.stride = *row_bytes;
  return source;
}

std::expected<RgbSource, std::string> rgb_source_for_encode(
    const ImageBuffer& image,
    const ImagePlane& plane,
    avifImage* avif_image,
    const NativeEncodeSettings& settings,
    int bit_depth) {
  RgbSource source{};
  const bool keep_alpha = preserve_alpha_for_encode(settings);
  const std::size_t channels = keep_alpha ? 4 : 3;
  avifRGBImageSetDefaults(&source.rgb, avif_image);
  source.rgb.format = keep_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  source.rgb.depth = static_cast<std::uint32_t>(bit_depth);
  source.rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
  if (bit_depth == 8) {
    if (keep_alpha) {
      source.rgb.pixels = reinterpret_cast<std::uint8_t*>(
          const_cast<std::byte*>(plane.bytes.data()));
      source.rgb.rowBytes = static_cast<std::uint32_t>(plane.stride);
      return source;
    }
    const auto row_bytes = checked_interleaved_stride(image.width, channels, "AVIF encoder");
    if (!row_bytes) {
      return std::unexpected{row_bytes.error()};
    }
    const auto image_bytes = checked_image_bytes(*row_bytes, image.height, "AVIF encoder");
    if (!image_bytes) {
      return std::unexpected{image_bytes.error()};
    }
    source.low_depth_pixels.resize(*image_bytes);
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = reinterpret_cast<const std::uint8_t*>(
          plane.bytes.data() + y * plane.stride);
      auto* out = source.low_depth_pixels.data() + y * *row_bytes;
      for (std::size_t x = 0; x < image.width; ++x) {
        out[x * 3 + 0] = row[x * 4 + 0];
        out[x * 3 + 1] = row[x * 4 + 1];
        out[x * 3 + 2] = row[x * 4 + 2];
      }
    }
    source.rgb.pixels = source.low_depth_pixels.data();
    source.rgb.rowBytes = static_cast<std::uint32_t>(*row_bytes);
    return source;
  }
  if (bit_depth != 10 && bit_depth != 12) {
    return std::unexpected{"AVIF encoder 只支持 8、10、12-bit 输出。"};
  }
  const auto pixel_count = checked_pixel_count(image.width, image.height, "AVIF encoder");
  if (!pixel_count) {
    return std::unexpected{pixel_count.error()};
  }
  if (*pixel_count > std::numeric_limits<std::size_t>::max() / channels) {
    return std::unexpected{"AVIF encoder 高位深临时 buffer 过大。"};
  }
  source.high_depth_pixels.resize(*pixel_count * channels);
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        plane.bytes.data() + y * plane.stride);
    auto* out = source.high_depth_pixels.data() + y * image.width * channels;
    for (std::size_t x = 0; x < image.width; ++x) {
      out[x * channels + 0] = expand_u8_to_depth(row[x * 4 + 0], bit_depth);
      out[x * channels + 1] = expand_u8_to_depth(row[x * 4 + 1], bit_depth);
      out[x * channels + 2] = expand_u8_to_depth(row[x * 4 + 2], bit_depth);
      if (keep_alpha) {
        out[x * channels + 3] = expand_u8_to_depth(row[x * 4 + 3], bit_depth);
      }
    }
  }
  const auto high_depth_stride = checked_interleaved_stride(
      image.width, channels, "AVIF encoder", sizeof(std::uint16_t));
  if (!high_depth_stride) {
    return std::unexpected{high_depth_stride.error()};
  }
  source.rgb.pixels = reinterpret_cast<std::uint8_t*>(source.high_depth_pixels.data());
  source.rgb.rowBytes = static_cast<std::uint32_t>(*high_depth_stride);
  return source;
}

avifCodecChoice codec_choice_for(AvifEncoderMode mode) noexcept {
  switch (mode) {
    case AvifEncoderMode::aom:
    case AvifEncoderMode::automatic:
    case AvifEncoderMode::svt:
    case AvifEncoderMode::zenrav1e:
    default:
      return AVIF_CODEC_CHOICE_AOM;
  }
}

std::string actual_libavif_id(AvifEncoderMode mode) {
  return avif_encoder_mode_name(mode == AvifEncoderMode::automatic ? AvifEncoderMode::aom : mode);
}

std::string libavif_codec_name_for(AvifEncoderMode mode) {
  return std::format("libavif-{}", actual_libavif_id(mode));
}

bool libavif_encoder_available(AvifEncoderMode mode) {
  return avifCodecName(codec_choice_for(mode), AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr;
}

bool lossless_requested(const NativeEncodeSettings& settings) noexcept {
  return settings.quality >= 100 || settings.visual_quality == 100;
}

SpeedMapping libavif_speed_mapping(AvifEncoderMode, int speed) {
  speed = std::clamp(speed, 0, 10);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = speed,
                      .codec_key = "aom:cpu-used"};
}

PixelFormat pixel_format_from_avif(avifPixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case AVIF_PIXEL_FORMAT_YUV444:
      return PixelFormat::yuv444;
    case AVIF_PIXEL_FORMAT_YUV422:
      return PixelFormat::yuv422;
    case AVIF_PIXEL_FORMAT_YUV420:
    case AVIF_PIXEL_FORMAT_YUV400:
      return PixelFormat::yuv420;
    case AVIF_PIXEL_FORMAT_NONE:
    default:
      return PixelFormat::unknown;
  }
}

std::string avif_error(avifResult result, const avifEncoder* encoder = nullptr) {
  if (encoder != nullptr && encoder->diag.error[0] != '\0') {
    return std::format("{}: {}", avifResultToString(result), encoder->diag.error);
  }
  return avifResultToString(result);
}

std::string avif_decode_error(avifResult result, const avifDecoder* decoder = nullptr) {
  if (decoder != nullptr && decoder->diag.error[0] != '\0') {
    return std::format("{}: {}", avifResultToString(result), decoder->diag.error);
  }
  return avifResultToString(result);
}

}  // namespace avif_aom_detail

export bool avif_libavif_encoder_available(AvifEncoderMode mode) {
  return mode != AvifEncoderMode::svt && mode != AvifEncoderMode::zenrav1e &&
         avif_aom_detail::libavif_encoder_available(mode);
}

export bool avif_zenravif_encoder_available() noexcept {
#if AWJ_HAS_ZENRAVIF
  return true;
#else
  return false;
#endif
}

export bool avif_svtav1hdr_encoder_available() noexcept {
  return avifCodecName(AVIF_CODEC_CHOICE_SVT, AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr;
}

export std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_current_build(
    bool enable_experimental = false) {
  return avif_encoder_capabilities_for_build(
      avif_libavif_encoder_available(AvifEncoderMode::aom),
      avif_svtav1hdr_encoder_available(),
      avif_zenravif_encoder_available(),
      enable_experimental);
}

export std::expected<AvifEncoderSelection, std::string> select_avif_encoder_for_current_build(
    const AvifEncoderSelectionRequest& request,
    bool enable_experimental = false) {
  const auto capabilities = avif_encoder_capabilities_for_current_build(enable_experimental);
  return select_avif_encoder_from_capabilities(request, capabilities);
}

std::expected<NativeEncodeResult, std::string> encode_with_current_settings(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings) {
  const bool lossless = avif_aom_detail::lossless_requested(settings);
  const auto actual_mode = AvifEncoderMode::aom;
  const auto applied_chroma = avif_aom_detail::applied_chroma_from_settings(
      image, settings.chroma_mode, lossless);
  const auto pixel_format = avif_aom_detail::avif_pixel_format_from_chroma(
      applied_chroma);
  if (!pixel_format) {
    return std::unexpected{pixel_format.error()};
  }

  auto plane = avif_aom_detail::rgba8_plane(image, avif_encoder_mode_name(actual_mode));
  if (!plane) {
    return std::unexpected{plane.error()};
  }
  if (image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      (*plane)->stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected{"AVIF encoder 输入尺寸超过 libavif API 限制。"};
  }
  if (lossless && !settings.bit_depth && image.source_info &&
      image.source_info->bit_depth > 0 && image.source_info->bit_depth != 8 &&
      image.source_info->bit_depth != 10 && image.source_info->bit_depth != 12) {
    return std::unexpected{std::format(
        "AVIF 无损模式无法保持源图 {}-bit 位深；libavif AOM 当前仅支持 8、10、12-bit 输出。",
        image.source_info->bit_depth)};
  }
  const int bit_depth = avif_aom_detail::applied_bit_depth_from_settings(
      image, settings, lossless);
  if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
    return std::unexpected{"AVIF encoder 只支持 8、10、12-bit 输出。"};
  }

  avif_aom_detail::AvifEncoder encoder{avifEncoderCreate()};
  if (!encoder) {
    return std::unexpected{"无法创建 libavif encoder。"};
  }
  encoder->codecChoice = avif_aom_detail::codec_choice_for(actual_mode);
  encoder->quality = lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(settings.quality, 1, 100);
  encoder->qualityAlpha = lossless ? AVIF_QUALITY_LOSSLESS : 100;
  if (settings.speed_explicit) {
    encoder->speed = std::clamp(settings.speed, 0, 10);
  }
  encoder->keyframeInterval = 1;
  encoder->maxThreads = std::max(1, settings.resources.encoder_threads_per_file);

  const auto set_option = [&](std::string_view key, std::string_view value) -> std::expected<void, std::string> {
    const avifResult option_result = avifEncoderSetCodecSpecificOption(
        encoder.get(), std::string{key}.c_str(), std::string{value}.c_str());
    if (option_result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF AOM 设置参数 {}={} 失败: {}",
                                         key, value,
                                         avif_aom_detail::avif_error(option_result, encoder.get()))};
    }
    return {};
  };
  if (!lossless && settings.avif_tune_iq) {
    if (auto set = set_option("color:tune", "iq"); !set) {
      return std::unexpected{set.error()};
    }
  }

  avif_aom_detail::AvifRwData output{new avifRWData{}};
  output->data = nullptr;
  output->size = 0;

  avifResult result = AVIF_RESULT_OK;
  if (settings.avif_grid_plan) {
    const auto& plan = *settings.avif_grid_plan;
    if (plan.uses_padding) {
      return std::unexpected{
          "AVIF grid padding 尚未接入安全裁切，不能生成会扩大尺寸的 grid 输出。"};
    }
    if (plan.cols == 0 || plan.rows == 0 || plan.tile_width == 0 || plan.tile_height == 0) {
      return std::unexpected{"AVIF grid 规划无效。"};
    }
    const auto tile_count = static_cast<std::uint64_t>(plan.cols) * plan.rows;
    if (tile_count == 0 || tile_count > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected{"AVIF grid tile 数量过大。"};
    }

    std::vector<avif_aom_detail::AvifImage> tile_storage;
    std::vector<const avifImage*> tile_views;
    tile_storage.reserve(static_cast<std::size_t>(tile_count));
    tile_views.reserve(static_cast<std::size_t>(tile_count));
    std::vector<std::byte> tile_pixels;
    const auto tile_stride = avif_aom_detail::checked_rgba_stride(plan.tile_width, "AVIF grid");
    if (!tile_stride) {
      return std::unexpected{tile_stride.error()};
    }
    const auto tile_bytes = avif_aom_detail::checked_image_bytes(*tile_stride, plan.tile_height, "AVIF grid");
    if (!tile_bytes) {
      return std::unexpected{tile_bytes.error()};
    }

    tile_pixels.resize(*tile_bytes);

    for (std::uint32_t row = 0; row < plan.rows; ++row) {
      for (std::uint32_t col = 0; col < plan.cols; ++col) {
        const std::size_t src_x = static_cast<std::size_t>(col) * plan.tile_width;
        const std::size_t src_y = static_cast<std::size_t>(row) * plan.tile_height;
        for (std::uint32_t y = 0; y < plan.tile_height; ++y) {
          const auto source_y = src_y + y;
          auto* dst = tile_pixels.data() + static_cast<std::size_t>(y) * *tile_stride;
          const auto copy_width = std::min<std::size_t>(
              plan.tile_width, image.width > src_x ? image.width - src_x : 0);
          if (copy_width == 0) {
            continue;
          }
          const auto clamped_y = std::min<std::size_t>(source_y, image.height - 1);
          const auto* src = (*plane)->bytes.data() + clamped_y * (*plane)->stride + src_x * 4;
          std::ranges::copy_n(src, copy_width * 4, dst);
          if (copy_width < plan.tile_width) {
            const auto* last_pixel = dst + (copy_width - 1) * 4;
            for (std::size_t x = copy_width; x < plan.tile_width; ++x) {
              std::ranges::copy_n(last_pixel, 4, dst + x * 4);
            }
          }
        }
        auto tile = avif_aom_detail::AvifImage{avifImageCreate(
            plan.tile_width, plan.tile_height, bit_depth, *pixel_format)};
        if (!tile) {
          return std::unexpected{"无法创建 AVIF grid tile。"};
        }
        avif_aom_detail::apply_color_settings(*tile, settings, applied_chroma, lossless);
        ImagePlane tile_plane{.bytes = tile_pixels, .stride = *tile_stride};
        ImageBuffer tile_buffer{.width = plan.tile_width,
                                .height = plan.tile_height,
                                .pixel_format = PixelFormat::rgba,
                                .alpha_mode = image.alpha_mode,
                                .bit_depth = 8,
                                .planes = {std::move(tile_plane)}};
        auto rgb = avif_aom_detail::rgb_source_for_encode(
            tile_buffer, tile_buffer.planes.front(), tile.get(), settings, bit_depth);
        if (!rgb) {
          return std::unexpected{rgb.error()};
        }
        result = avifImageRGBToYUV(tile.get(), &rgb->rgb);
        if (result != AVIF_RESULT_OK) {
          return std::unexpected{std::format("AVIF grid tile RGB 转 YUV 失败: {}",
                                            avifResultToString(result))};
        }
        tile_views.push_back(tile.get());
        tile_storage.push_back(std::move(tile));
      }
    }

    result = avifEncoderAddImageGrid(
        encoder.get(), plan.cols, plan.rows,
        reinterpret_cast<const avifImage* const*>(tile_views.data()),
        AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF grid 编码失败: {}",
                                         avif_aom_detail::avif_error(result, encoder.get()))};
    }
    result = avifEncoderFinish(encoder.get(), output.get());
  } else {
    avif_aom_detail::AvifImage avif_image{avifImageCreate(
        static_cast<std::uint32_t>(image.width),
        static_cast<std::uint32_t>(image.height), bit_depth, *pixel_format)};
    if (!avif_image) {
      return std::unexpected{"无法创建 libavif image。"};
    }
    avif_aom_detail::apply_color_settings(*avif_image, settings, applied_chroma, lossless);

    auto rgb = avif_aom_detail::rgb_source_for_encode(image, **plane, avif_image.get(), settings, bit_depth);
    if (!rgb) {
      return std::unexpected{rgb.error()};
    }
    result = avifImageRGBToYUV(avif_image.get(), &rgb->rgb);
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF RGB 转 YUV 失败: {}",
                                         avifResultToString(result))};
    }
    result = avifEncoderWrite(encoder.get(), avif_image.get(), output.get());
  }
  if (result != AVIF_RESULT_OK) {
    return std::unexpected{std::format("AVIF {} 编码失败: {}",
                                       avif_encoder_mode_name(actual_mode),
                                       avif_aom_detail::avif_error(result, encoder.get()))};
  }
  if (output->size == 0 || output->data == nullptr) {
    return std::unexpected{"AVIF 编码输出为空。"};
  }

  EncodedImage encoded{.codec_name = avif_aom_detail::libavif_codec_name_for(actual_mode)};
  encoded.bytes.resize(output->size);
  std::ranges::copy_n(reinterpret_cast<std::byte*>(output->data), output->size,
                      encoded.bytes.begin());

  auto diagnostics = diagnostics_from_settings(settings);
  diagnostics.encoder_id = avif_encoder_mode_name(actual_mode);
  diagnostics.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  diagnostics.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  diagnostics.applied_chroma = chroma_mode_name(applied_chroma);
  diagnostics.requested_bit_depth = settings.requested_bit_depth;
  diagnostics.applied_bit_depth = bit_depth;
  diagnostics.bit_depth_reason = settings.bit_depth_reason.empty()
                                     ? avif_aom_detail::default_bit_depth_reason(
                                           image, settings, lossless)
                                     : settings.bit_depth_reason;
  diagnostics.fallback_reason = settings.encoder_fallback_reason;
  diagnostics.encoder_license = "BSD-2-Clause";
  diagnostics.integration_mode = settings.avif_grid_plan ? "libavif-grid" : std::string{};
  diagnostics.speed_mapping = settings.speed_explicit
                                  ? avif_aom_detail::libavif_speed_mapping(actual_mode, settings.speed)
                                  : SpeedMapping{.user_speed = settings.speed,
                                                 .codec_value = -1,
                                                 .codec_key = "aom:encoder-default"};
  diagnostics.encoder_threads = settings.resources.encoder_threads_per_file;
  diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;

  return NativeEncodeResult{.encoded = std::move(encoded),
                            .diagnostics = std::move(diagnostics),
                            .final_quality = std::clamp(settings.quality, 1, 100),
                            .lossless = lossless,
                            .search_attempt_count = 1};
}

export class AvifLibavifImageEncoder final : public ImageEncoder {
 public:
  explicit AvifLibavifImageEncoder(AvifEncoderMode mode) : mode_{mode} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return "aom";
  }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::avif,
                             .features = CodecFeature::alpha |
                                         CodecFeature::thread_control,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8, 10, 12}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
    if (!avif_aom_detail::libavif_encoder_available(mode_)) {
      return std::unexpected{std::format(
          "AVIF encoder {} is not available in this libavif build.",
          avif_encoder_mode_name(mode_))};
    }
    return encode_with_current_settings(image, settings);
  }

 private:
  AvifEncoderMode mode_{};
};

export class AvifAomImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return impl_.id(); }
  [[nodiscard]] CodecCapabilities capabilities() const override { return impl_.capabilities(); }
  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
    return impl_.encode(image, settings);
  }

 private:
  AvifLibavifImageEncoder impl_{AvifEncoderMode::aom};
};

export class ZenravifImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "zenrav1e"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::avif,
                             .features = CodecFeature::alpha |
                                         CodecFeature::thread_control,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 1,
                             .max_speed = 10,
                             .bit_depths = {8, 10, 12}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
#if !AWJ_HAS_ZENRAVIF
    (void)image;
    (void)settings;
    return std::unexpected{"AVIF encoder zenrav1e is not available in this build; the zenravif bridge was not built."};
#else
    if (avif_aom_detail::lossless_requested(settings)) {
      return std::unexpected{
          "zenrav1e 无损 AVIF 重编码不能保证继承全部源图参数；请使用 --avif-encoder auto/aom。"};
    }
    auto plane = avif_aom_detail::rgba8_plane(image, "zenravif");
    if (!plane) {
      return std::unexpected{plane.error()};
    }
    const int bit_depth = settings.bit_depth.value_or(8);
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
      return std::unexpected{"zenravif encoder 只支持 8、10、12-bit 输出。"};
    }
    const auto applied_chroma = avif_aom_detail::applied_chroma_from_settings(
        image, settings.chroma_mode, false);
    if (applied_chroma != ChromaMode::yuv420 && applied_chroma != ChromaMode::yuv444) {
      return std::unexpected{"zenravif encoder only supports 420 or 444 chroma."};
    }

    auto bridge_source = avif_aom_detail::rgba8_source_for_bridge(
        image, **plane, settings, "zenravif");
    if (!bridge_source) {
      return std::unexpected{bridge_source.error()};
    }

    avif_aom_detail::ZenravifBytes output{};
    std::array<std::uint8_t, 512> error{};
    const int speed = std::clamp(settings.speed, 1, 10);
    const int code = zenravif_bridge_encode_rgba8(
        bridge_source->pixels,
        image.width, image.height, bridge_source->stride,
        std::clamp(settings.quality, 1, 100), speed, bit_depth,
        avif_aom_detail::chroma_numeric(applied_chroma),
        static_cast<std::size_t>(std::max(1, settings.resources.encoder_threads_per_file)),
        encoding_defaults::default_zenrav1e_keyint,
        encoding_defaults::default_zenrav1e_still_picture,
        encoding_defaults::default_zenrav1e_enable_qm,
        encoding_defaults::default_zenrav1e_vaq_strength,
        encoding_defaults::default_zenrav1e_enable_trellis,
        encoding_defaults::default_zenrav1e_rdo_tx_decision,
        &output.output, error.data(), error.size());
    if (code != 0) {
      error.back() = '\0';
      return std::unexpected{std::format("zenravif 编码失败: {}",
                                         reinterpret_cast<const char*>(error.data()))};
    }
    if (output.output.data == nullptr || output.output.size == 0) {
      return std::unexpected{"zenravif 编码输出为空。"};
    }

    EncodedImage encoded{.codec_name = "zenravif"};
    encoded.bytes.resize(output.output.size);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(output.output.data), output.output.size,
                        encoded.bytes.begin());

    auto diagnostics = diagnostics_from_settings(settings);
    diagnostics.encoder_id = "zenrav1e";
    diagnostics.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
    diagnostics.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
    diagnostics.applied_chroma = chroma_mode_name(applied_chroma);
    diagnostics.requested_bit_depth = settings.requested_bit_depth;
    diagnostics.applied_bit_depth = bit_depth;
    diagnostics.bit_depth_reason = settings.bit_depth_reason.empty()
                                       ? (settings.bit_depth_explicit ? "explicit bit-depth requested"
                                                                      : "auto selected encoder default bit-depth")
                                       : settings.bit_depth_reason;
    diagnostics.fallback_reason = settings.encoder_fallback_reason;
    diagnostics.encoder_experimental = true;
    diagnostics.encoder_license = "AGPL-3.0-only OR LicenseRef-Imazen-Commercial";
    diagnostics.color_metadata_source = "zenravif-bridge-default";
    diagnostics.color_reason = "zenravif bridge does not expose CICP/HDR metadata controls";
    diagnostics.applied_icc = settings.source_has_icc ? "not-written" : "none";
    diagnostics.applied_hdr_metadata = settings.source_has_hdr_metadata ? "not-written" : "none";
    diagnostics.speed_mapping = SpeedMapping{.user_speed = speed,
                                             .codec_value = speed,
                                             .codec_key = "zenravif:speed"};
    diagnostics.encoder_threads = settings.resources.encoder_threads_per_file;
    diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;

    return NativeEncodeResult{.encoded = std::move(encoded),
                              .diagnostics = std::move(diagnostics),
                              .final_quality = std::clamp(settings.quality, 1, 100),
                              .lossless = avif_aom_detail::lossless_requested(settings),
                              .search_attempt_count = 1};
#endif
  }
};

export class AvifImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libavif"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".avif";
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = avif_aom_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
    if (!decoder) {
      return std::unexpected{"无法创建 libavif decoder。"};
    }
    decoder->codecChoice = AVIF_CODEC_CHOICE_AUTO;
    decoder->maxThreads = 1;
    avifResult result = avifDecoderSetIOMemory(
        decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes->data()),
        bytes->size());
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF 设置输入失败: {}",
                                         avif_aom_detail::avif_decode_error(result, decoder.get()))};
    }
    result = avifDecoderParse(decoder.get());
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF 读取尺寸失败: {}",
                                         avif_aom_detail::avif_decode_error(result, decoder.get()))};
    }
    if (decoder->image == nullptr) {
      return std::unexpected{std::format("AVIF 图像信息为空: {}", path_to_utf8(path))};
    }
    return decoder_common::make_image_dimensions_checked(decoder->image->width,
                                                         decoder->image->height,
                                                         "AVIF");
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    auto bytes = avif_aom_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
    if (!decoder) {
      return std::unexpected{"无法创建 libavif decoder。"};
    }
    decoder->codecChoice = AVIF_CODEC_CHOICE_AUTO;
    decoder->maxThreads = 1;

    avif_aom_detail::AvifImage image{avifImageCreateEmpty()};
    if (!image) {
      return std::unexpected{"无法创建 libavif decode image。"};
    }
    auto result = avifDecoderReadMemory(
        decoder.get(), image.get(), reinterpret_cast<const std::uint8_t*>(bytes->data()),
        bytes->size());
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF 解码失败: {}",
                                         avif_aom_detail::avif_decode_error(result, decoder.get()))};
    }

    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, image.get());
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.maxThreads = 1;
    result = avifRGBImageAllocatePixels(&rgb);
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF RGB buffer 分配失败: {}",
                                         avifResultToString(result))};
    }
    avif_aom_detail::AvifRgbPixels rgb_guard{&rgb};
    result = avifImageYUVToRGB(image.get(), &rgb);
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF YUV 转 RGB 失败: {}",
                                         avifResultToString(result))};
    }

    const auto byte_count = avif_aom_detail::checked_image_bytes(
        static_cast<std::size_t>(rgb.rowBytes), static_cast<std::size_t>(rgb.height), "AVIF decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    ImagePlane plane{.stride = rgb.rowBytes};
    plane.bytes.resize(*byte_count);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(rgb.pixels), *byte_count,
                        plane.bytes.begin());
    ImageBuffer out{.width = rgb.width,
                    .height = rgb.height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = image->alphaPlane != nullptr ? AlphaMode::straight : AlphaMode::none,
                    .bit_depth = 8,
                    .source_info = ImageSourceInfo{
                        .pixel_format = avif_aom_detail::pixel_format_from_avif(image->yuvFormat),
                        .bit_depth = static_cast<int>(image->depth),
                        .color_primaries = avif_aom_detail::int_from_avif_color(image->colorPrimaries),
                        .transfer_characteristics = avif_aom_detail::int_from_avif_transfer(
                            image->transferCharacteristics),
                        .matrix_coefficients = avif_aom_detail::int_from_avif_matrix(
                            image->matrixCoefficients),
                        .color_range = avif_aom_detail::int_from_avif_range(image->yuvRange),
                        .has_hdr_metadata = avif_aom_detail::has_hdr_cicp(*image),
                        .color_metadata_source = avif_aom_detail::color_metadata_source_from_avif(
                            *image)}};
    if (avif_aom_detail::has_avif_icc(*image)) {
      MetadataBlock icc{.kind = MetadataKind::icc};
      icc.bytes.resize(image->icc.size);
      std::ranges::copy_n(reinterpret_cast<std::byte*>(image->icc.data), image->icc.size,
                          icc.bytes.begin());
      out.metadata.push_back(std::move(icc));
    }
    out.planes.push_back(std::move(plane));
    return ImageDecodeResult{.image = std::move(out), .decoder_id = "libavif"};
  }
};

}  // namespace awj
