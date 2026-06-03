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
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
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
import awj.decoder_common;
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

std::expected<void, std::string> stop_if_requested(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return std::unexpected{"任务已取消。"};
  }
  return {};
}

std::expected<AvifRwData, std::string> make_avif_rw_data() {
  try {
    auto data = std::make_unique<avifRWData>();
    data->data = nullptr;
    data->size = 0;
    return AvifRwData{data.release()};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"svt-av1-hdr 输出缓冲区内存不足。"};
  }
}

constexpr std::size_t max_metadata_bytes = 64 * 1024 * 1024;

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
  if (static_cast<std::uint64_t>(required) > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{"svt-av1-hdr 输入 RGBA buffer 超过 20 GiB 运行时上限。"};
  }
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

template <typename T>
std::expected<std::vector<T>, std::string> make_typed_buffer(std::size_t count,
                                                             std::string_view context,
                                                             std::string_view label) {
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    return std::unexpected{std::format("{} {} 尺寸超过运行时限制。", context, label)};
  }
  const auto byte_count = count * sizeof(T);
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{std::format("{} {} 超过 20 GiB 运行时上限。", context, label)};
  }
  std::vector<T> buffer;
  try {
    buffer.resize(count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"SVT-AV1-HDR 编码缓冲区内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"SVT-AV1-HDR 编码缓冲区尺寸超过运行时限制。"};
  }
  return buffer;
}

std::expected<std::vector<std::byte>, std::string> copy_avif_output(
    const avifRWData& output,
    std::stop_token stop_token = {}) {
  if (output.size > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{"svt-av1-hdr 输出 AVIF 超过 20 GiB 运行时上限。"};
  }
  auto bytes = decoder_common::make_byte_buffer(output.size, "svt-av1-hdr");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  std::memcpy(bytes->data(), output.data, output.size);
  return bytes;
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

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

const MetadataBlock* first_icc_metadata(const ImageBuffer& image) noexcept {
  return first_metadata(image, MetadataKind::icc);
}

std::expected<void, std::string> set_avif_metadata(
    avifResult result,
    std::string_view kind) {
  if (result != AVIF_RESULT_OK) {
    return std::unexpected{std::format("svt-av1-hdr 设置 {} 元数据失败: {}",
                                       kind, avifResultToString(result))};
  }
  return {};
}

std::expected<void, std::string> ensure_metadata_size(std::size_t size,
                                                      std::string_view context) {
  if (size > max_metadata_bytes) {
    return std::unexpected{std::format("svt-av1-hdr {} 元数据超过 64 MiB 上限。", context)};
  }
  return {};
}

int codec_thread_count(int requested_threads) noexcept {
  return std::clamp(requested_threads, 1, encoding_defaults::default_av1_encoder_thread_cap);
}

std::expected<void, std::string> validate_int_range(int value,
                                                    int min_value,
                                                    int max_value,
                                                    std::string_view name) {
  if (value < min_value || value > max_value) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, min_value, max_value)};
  }
  return {};
}

std::expected<void, std::string> validate_optional_int_range(std::optional<int> value,
                                                             int min_value,
                                                             int max_value,
                                                             std::string_view name) {
  if (!value) {
    return {};
  }
  return validate_int_range(*value, min_value, max_value, name);
}

std::expected<void, std::string> validate_svtav1hdr_ascii_text(std::string_view value,
                                                               std::string_view name) {
  constexpr std::size_t max_value_length = 512;
  if (value.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  if (value.size() > max_value_length) {
    return std::unexpected{std::format("{} 长度不能超过 512 个字符。", name)};
  }
  for (const char raw : value) {
    const auto ch = static_cast<unsigned char>(raw);
    if (ch < 0x20 || ch == 0x7f || ch > 0x7f) {
      return std::unexpected{std::format("{} 只能包含 ASCII 非控制字符。", name)};
    }
  }
  return {};
}

std::expected<void, std::string> validate_svtav1hdr_wide_text(std::wstring_view value,
                                                              std::string_view name) {
  constexpr std::size_t max_value_length = 512;
  if (value.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  if (value.size() > max_value_length) {
    return std::unexpected{std::format("{} 长度不能超过 512 个字符。", name)};
  }
  for (const wchar_t ch : value) {
    if (ch < 0x20 || ch == 0x7f) {
      return std::unexpected{std::format("{} 不能包含控制字符。", name)};
    }
  }
  return {};
}

std::expected<void, std::string> validate_svtav1hdr_key_value_option(std::wstring_view value,
                                                                     std::string_view name) {
  if (auto valid = validate_svtav1hdr_wide_text(value, name); !valid) {
    return std::unexpected{valid.error()};
  }
  const auto equals = value.find(L'=');
  if (equals == std::wstring_view::npos || equals == 0) {
    return std::unexpected{std::format("{} 必须为 key=value。", name)};
  }
  return {};
}

std::expected<void, std::string> validate_svtav1hdr_settings(
    const SvtAv1HdrSettings& settings) {
  if (auto valid = validate_optional_int_range(settings.crf, 0, 63, "svtav1hdr-crf"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_int_range(settings.preset, 0, 13, "svtav1hdr-preset"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_svtav1hdr_ascii_text(settings.tune, "svtav1hdr-tune"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_int_range(settings.keyint, 1, 999999, "svtav1hdr-keyint"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(settings.color_primaries, 0, 255, "color-primaries"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(settings.transfer_characteristics, 0, 255, "transfer-characteristics"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(settings.matrix_coefficients, 0, 255, "matrix-coefficients"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(settings.color_range, 0, 1, "color-range"); !valid) {
    return std::unexpected{valid.error()};
  }
  if (!settings.mastering_display.empty()) {
    if (auto valid = validate_svtav1hdr_wide_text(settings.mastering_display, "mastering-display"); !valid) {
      return std::unexpected{valid.error()};
    }
  }
  if (!settings.content_light.empty()) {
    if (auto valid = validate_svtav1hdr_wide_text(settings.content_light, "content-light"); !valid) {
      return std::unexpected{valid.error()};
    }
  }
  for (const auto& param : settings.params) {
    if (auto valid = validate_svtav1hdr_key_value_option(param, "--svtav1hdr-params"); !valid) {
      return std::unexpected{valid.error()};
    }
  }
  return {};
}

std::expected<void, std::string> apply_icc_profile(avifImage& avif_image,
                                                   const ImageBuffer& image,
                                                   const NativeEncodeSettings& settings) {
  if (settings.strip_metadata || settings.applied_icc != "kept") {
    return {};
  }
  const auto* icc = first_icc_metadata(image);
  if (icc == nullptr) {
    return {};
  }
  if (auto checked = ensure_metadata_size(icc->bytes.size(), "ICC profile"); !checked) {
    return std::unexpected{checked.error()};
  }
  const auto result = avifImageSetProfileICC(
      &avif_image, reinterpret_cast<const std::uint8_t*>(icc->bytes.data()), icc->bytes.size());
  if (result != AVIF_RESULT_OK) {
    return std::unexpected{std::format("svt-av1-hdr 设置 ICC profile 失败: {}", avifResultToString(result))};
  }
  return {};
}

std::expected<void, std::string> apply_content_light_metadata(avifImage& avif_image,
                                                              const NativeEncodeSettings& settings) {
  if (settings.strip_metadata || settings.applied_hdr_metadata != "kept" ||
      !settings.source_content_light) {
    return {};
  }
  avif_image.clli.maxCLL = settings.source_content_light->max_cll;
  avif_image.clli.maxPALL = settings.source_content_light->max_pall;
  return {};
}

std::expected<void, std::string> apply_avif_metadata(avifImage& avif_image,
                                                       const ImageBuffer& image,
                                                       const NativeEncodeSettings& settings) {
  if (auto icc = apply_icc_profile(avif_image, image, settings); !icc) {
    return std::unexpected{icc.error()};
  }
  if (auto content_light = apply_content_light_metadata(avif_image, settings); !content_light) {
    return std::unexpected{content_light.error()};
  }
  if (settings.strip_metadata) {
    return {};
  }
  if (const auto* exif = first_metadata(image, MetadataKind::exif)) {
    if (auto checked = ensure_metadata_size(exif->bytes.size(), "Exif"); !checked) {
      return std::unexpected{checked.error()};
    }
    if (auto set = set_avif_metadata(
            avifImageSetMetadataExif(&avif_image,
                                      reinterpret_cast<const std::uint8_t*>(exif->bytes.data()),
                                      exif->bytes.size()),
            "Exif"); !set) {
      return std::unexpected{set.error()};
    }
  }
  if (const auto* xmp = first_metadata(image, MetadataKind::xmp)) {
    if (auto checked = ensure_metadata_size(xmp->bytes.size(), "XMP"); !checked) {
      return std::unexpected{checked.error()};
    }
    if (auto set = set_avif_metadata(
            avifImageSetMetadataXMP(&avif_image,
                                    reinterpret_cast<const std::uint8_t*>(xmp->bytes.data()),
                                    xmp->bytes.size()),
            "XMP"); !set) {
      return std::unexpected{set.error()};
    }
  }
  return {};
}

std::expected<AvifImage, std::string> avif_image_from_rgba(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    int bit_depth,
    std::stop_token stop_token = {}) {
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  auto plane_view = rgba8_plane(image);
  if (!plane_view) {
    return std::unexpected{plane_view.error()};
  }
  const auto* plane = plane_view->plane;
  if (image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      plane->stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected{"svt-av1-hdr 输入尺寸超过 libavif API 限制。"};
  }
  if (image.width > static_cast<std::size_t>(encoding_defaults::avif_single_image_max_dimension) ||
      image.height > static_cast<std::size_t>(encoding_defaults::avif_single_image_max_dimension)) {
    return std::unexpected{std::format(
        "svt-av1-hdr 单图 AVIF 输入尺寸 {}x{} 超过边长上限 {}。",
        image.width, image.height, encoding_defaults::avif_single_image_max_dimension)};
  }

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
  if (auto metadata = apply_avif_metadata(*avif_image, image, settings); !metadata) {
    return std::unexpected{metadata.error()};
  }

  avifRGBImage rgb{};
  avifRGBImageSetDefaults(&rgb, avif_image.get());
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.depth = static_cast<std::uint32_t>(bit_depth);
  rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
  if (bit_depth == 8) {
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.ignoreAlpha = AVIF_TRUE;
    rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(plane->bytes.data()));
    rgb.rowBytes = static_cast<std::uint32_t>(plane->stride);
    if (auto stopped = stop_if_requested(stop_token); !stopped) {
      return std::unexpected{stopped.error()};
    }
    const avifResult converted = avifImageRGBToYUV(avif_image.get(), &rgb);
    if (auto stopped = stop_if_requested(stop_token); !stopped) {
      return std::unexpected{stopped.error()};
    }
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
  auto high_depth_pixels = make_typed_buffer<std::uint16_t>(
      *pixel_count * 3, "svt-av1-hdr", "高位深临时 buffer");
  if (!high_depth_pixels) {
    return std::unexpected{high_depth_pixels.error()};
  }
  for (std::size_t y = 0; y < image.height; ++y) {
    if (auto stopped = stop_if_requested(stop_token); !stopped) {
      return std::unexpected{stopped.error()};
    }
    const auto* row = reinterpret_cast<const std::uint8_t*>(plane->bytes.data() + y * plane->stride);
    auto* out = high_depth_pixels->data() + y * image.width * 3;
    for (std::size_t x = 0; x < image.width; ++x) {
      out[x * 3 + 0] = expand_u8_to_depth(row[x * 4 + 0], bit_depth);
      out[x * 3 + 1] = expand_u8_to_depth(row[x * 4 + 1], bit_depth);
      out[x * 3 + 2] = expand_u8_to_depth(row[x * 4 + 2], bit_depth);
    }
  }
  rgb.pixels = reinterpret_cast<std::uint8_t*>(high_depth_pixels->data());
  rgb.rowBytes = static_cast<std::uint32_t>(*row_bytes);
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  const avifResult converted = avifImageRGBToYUV(avif_image.get(), &rgb);
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  if (converted != AVIF_RESULT_OK) {
    return std::unexpected{std::format("svt-av1-hdr RGB 转 YUV 失败: {}", avifResultToString(converted))};
  }
  return avif_image;
}

std::expected<std::vector<std::byte>, std::string> encode_avif_with_svt_backend(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    int bit_depth,
    std::stop_token stop_token = {}) {
  auto avif_image = avif_image_from_rgba(image, settings, bit_depth, stop_token);
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
  const int encoder_threads = codec_thread_count(settings.resources.encoder_threads_per_file);
  encoder->maxThreads = encoder_threads;
  encoder->keyframeInterval = settings.svtav1hdr.keyint;

  const auto set_option = [&](std::string_view key, std::string_view value) -> std::expected<void, std::string> {
    if (auto stopped = stop_if_requested(stop_token); !stopped) {
      return std::unexpected{stopped.error()};
    }
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
  if (!settings.svtav1hdr.mastering_display.empty()) {
    const auto value = utf8_from_wide(settings.svtav1hdr.mastering_display);
    if (auto set = set_option("mastering-display", value); !set) {
      return std::unexpected{set.error()};
    }
  }
  if (!settings.svtav1hdr.content_light.empty()) {
    const auto value = utf8_from_wide(settings.svtav1hdr.content_light);
    if (auto set = set_option("content-light", value); !set) {
      return std::unexpected{set.error()};
    }
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

  auto output_holder = make_avif_rw_data();
  if (!output_holder) {
    return std::unexpected{output_holder.error()};
  }
  auto output = std::move(*output_holder);
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  const avifResult result = avifEncoderWrite(encoder.get(), (*avif_image).get(), output.get());
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return std::unexpected{stopped.error()};
  }
  if (result != AVIF_RESULT_OK) {
    return std::unexpected{svt_error_message("编码", *encoder, result)};
  }
  if (output->size == 0 || output->data == nullptr) {
    return std::unexpected{"svt-av1-hdr 输出 AVIF 为空。"};
  }
  return copy_avif_output(*output, stop_token);
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
    const NativeEncodeSettings& settings,
    std::stop_token stop_token = {}) {
#if !AWJ_HAS_SVTAV1HDR_STATIC
  (void)image;
  (void)settings;
  (void)stop_token;
  return std::unexpected{"svt-av1-hdr static backend 在当前构建中不可用。"};
#else
  try {
    if (auto stopped = svtav1hdr_detail::stop_if_requested(stop_token); !stopped) {
      return std::unexpected{stopped.error()};
    }
    if (auto valid = svtav1hdr_detail::validate_svtav1hdr_settings(settings.svtav1hdr); !valid) {
      return std::unexpected{valid.error()};
    }
    if (settings.chroma_mode != ChromaMode::yuv420) {
      return std::unexpected{"svt-av1-hdr 只支持 420 chroma。"};
    }
    const int bit_depth = settings.bit_depth.value_or(8);
    if (bit_depth != 8 && bit_depth != 10) {
      return std::unexpected{"svt-av1-hdr 当前只支持 8-bit 或 10-bit 输出。"};
    }
    if (!svtav1hdr_encoder_build_available()) {
      return std::unexpected{"svt-av1-hdr static backend 当前未包含 libavif SVT encoder 支持。"};
    }

    auto bytes = svtav1hdr_detail::encode_avif_with_svt_backend(image, settings, bit_depth,
                                                                 stop_token);
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
                                       ? (settings.requested_bit_depth ? "用户明确请求 bit-depth"
                                                                       : "auto 选择 svt-av1-hdr bit-depth")
                                       : settings.bit_depth_reason;
    diagnostics.fallback_reason = settings.encoder_fallback_reason;
    diagnostics.encoder_license = "BSD-3-Clause";
    diagnostics.integration_mode = "static-svtav1hdr";
    diagnostics.svtav1hdr_crf = settings.svtav1hdr.crf;
    diagnostics.svtav1hdr_preset = settings.svtav1hdr.preset;
    diagnostics.svtav1hdr_tune = settings.svtav1hdr.tune;
    diagnostics.svtav1hdr_keyint = settings.svtav1hdr.keyint;
    diagnostics.svtav1hdr_hdr_metadata = hdr_summary;
    diagnostics.speed_mapping = SpeedMapping{.user_speed = settings.speed,
                                             .codec_value = settings.svtav1hdr.preset,
                                             .codec_key = "svt-av1-hdr:preset"};
    diagnostics.encoder_threads = svtav1hdr_detail::codec_thread_count(
        settings.resources.encoder_threads_per_file);
    diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;

    const int final_quality = settings.svtav1hdr.crf
                                  ? svtav1hdr_detail::avif_quality_from_crf(*settings.svtav1hdr.crf)
                                  : std::clamp(settings.quality, 1, 100);
    const bool highest_quality = settings.visual_quality ? *settings.visual_quality >= 100 : final_quality >= 100;
    diagnostics.svtav1hdr_note = highest_quality
                                     ? "svt-av1-hdr q100 为非像素级无损/最高质量路径，允许 RGB/YUV 与 420 chroma 转换损耗。"
                                     : "svt-av1-hdr 通过 libavif SVT backend 静态链接";
    return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*bytes),
                                                      .codec_name = "svt-av1-hdr"},
                              .diagnostics = std::move(diagnostics),
                              .final_quality = final_quality,
                              .lossless = false,
                              .search_attempt_count = 1};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"svt-av1-hdr 编码内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"svt-av1-hdr 编码数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"svt-av1-hdr 编码文件系统访问失败。"};
  }
#endif
}

}  // namespace awj
