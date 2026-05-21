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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <avif/avif.h>

export module awj.avif_aom_codec;

import awj.codec;
import awj.config;
import awj.image;
import awj.avif_registry;

#ifndef AWJ_HAS_ZENRAVIF
#define AWJ_HAS_ZENRAVIF 0
#endif

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
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 AVIF 文件: {}", path.string())};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0) {
    return std::unexpected{std::format("AVIF 文件为空: {}", path.string())};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) {
    return std::unexpected{std::format("读取 AVIF 文件失败: {}", path.string())};
  }
  return bytes;
}

std::expected<const ImagePlane*, std::string> rgba8_plane(const ImageBuffer& image,
                                                          std::string_view encoder_id) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{std::format("{} encoder 当前需要 8-bit RGBA ImageBuffer。", encoder_id)};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = image.width * 4;
  if (plane.stride < expected_stride ||
      plane.bytes.size() < plane.stride * image.height) {
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

ChromaMode applied_chroma_from_settings(ChromaMode chroma) noexcept {
  return chroma == ChromaMode::auto_keep ? ChromaMode::yuv420 : chroma;
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
  std::vector<std::uint16_t> high_depth_pixels{};
};

std::expected<RgbSource, std::string> rgb_source_for_encode(
    const ImageBuffer& image,
    const ImagePlane& plane,
    avifImage* avif_image,
    int bit_depth) {
  RgbSource source{};
  avifRGBImageSetDefaults(&source.rgb, avif_image);
  source.rgb.format = AVIF_RGB_FORMAT_RGBA;
  source.rgb.depth = static_cast<std::uint32_t>(bit_depth);
  source.rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
  if (bit_depth == 8) {
    source.rgb.pixels = reinterpret_cast<std::uint8_t*>(
        const_cast<std::byte*>(plane.bytes.data()));
    source.rgb.rowBytes = static_cast<std::uint32_t>(plane.stride);
    return source;
  }
  if (bit_depth != 10 && bit_depth != 12) {
    return std::unexpected{"AVIF encoder 只支持 8、10、12-bit 输出。"};
  }
  source.high_depth_pixels.resize(image.width * image.height * 4);
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        plane.bytes.data() + y * plane.stride);
    auto* out = source.high_depth_pixels.data() + y * image.width * 4;
    for (std::size_t x = 0; x < image.width; ++x) {
      out[x * 4 + 0] = expand_u8_to_depth(row[x * 4 + 0], bit_depth);
      out[x * 4 + 1] = expand_u8_to_depth(row[x * 4 + 1], bit_depth);
      out[x * 4 + 2] = expand_u8_to_depth(row[x * 4 + 2], bit_depth);
      out[x * 4 + 3] = expand_u8_to_depth(row[x * 4 + 3], bit_depth);
    }
  }
  source.rgb.pixels = reinterpret_cast<std::uint8_t*>(source.high_depth_pixels.data());
  source.rgb.rowBytes = static_cast<std::uint32_t>(image.width * 4 * sizeof(std::uint16_t));
  return source;
}

avifCodecChoice codec_choice_for(AvifEncoderMode mode) noexcept {
  switch (mode) {
    case AvifEncoderMode::svt:
      return AVIF_CODEC_CHOICE_SVT;
    case AvifEncoderMode::aom:
    case AvifEncoderMode::automatic:
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

SpeedMapping libavif_speed_mapping(AvifEncoderMode mode, int speed) {
  speed = std::clamp(speed, 0, 10);
  if (mode == AvifEncoderMode::svt) {
    return map_avif_speed_to_svt_preset(speed);
  }
  return SpeedMapping{.user_speed = speed,
                      .codec_value = speed,
                      .codec_key = "aom:cpu-used"};
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

}  // namespace awj_aom_detail

export bool avif_libavif_encoder_available(AvifEncoderMode mode) {
  return mode != AvifEncoderMode::zenrav1e &&
         avif_aom_detail::libavif_encoder_available(mode);
}

export bool avif_zenravif_encoder_available() noexcept {
#if AWJ_HAS_ZENRAVIF
  return true;
#else
  return false;
#endif
}

export std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_current_build(
    bool enable_experimental = false) {
  return avif_encoder_capabilities_for_build(
      avif_libavif_encoder_available(AvifEncoderMode::aom),
      avif_libavif_encoder_available(AvifEncoderMode::svt),
      avif_zenravif_encoder_available(),
      enable_experimental);
}

export std::expected<AvifEncoderSelection, std::string> select_avif_encoder_for_current_build(
    const AvifEncoderSelectionRequest& request,
    bool enable_experimental = false) {
  const auto capabilities = avif_encoder_capabilities_for_current_build(enable_experimental);
  return select_avif_encoder_from_capabilities(request, capabilities);
}

export class AvifLibavifImageEncoder final : public ImageEncoder {
 public:
  explicit AvifLibavifImageEncoder(AvifEncoderMode mode) : mode_{mode} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return mode_ == AvifEncoderMode::svt ? "svt" : "aom";
  }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::avif,
                             .features = CodecFeature::alpha |
                                         CodecFeature::thread_control,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = mode_ == AvifEncoderMode::svt
                                               ? std::vector<int>{8, 10}
                                               : std::vector<int>{8, 10, 12}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
    const auto actual_mode = mode_ == AvifEncoderMode::automatic ? AvifEncoderMode::aom : mode_;
    if (!avif_aom_detail::libavif_encoder_available(actual_mode)) {
      return std::unexpected{std::format(
          "AVIF encoder {} is not available in this libavif build.",
          avif_encoder_mode_name(actual_mode))};
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
    const int bit_depth = settings.bit_depth.value_or(8);
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
      return std::unexpected{"AVIF encoder 只支持 8、10、12-bit 输出。"};
    }
    if (actual_mode == AvifEncoderMode::svt && bit_depth == 12) {
      return std::unexpected{"SVT AVIF encoder 当前不支持 12-bit 输出。"};
    }

    const auto applied_chroma = avif_aom_detail::applied_chroma_from_settings(
        settings.chroma_mode);
    if (actual_mode == AvifEncoderMode::svt && applied_chroma != ChromaMode::yuv420) {
      return std::unexpected{"SVT AVIF encoder only supports 420 chroma."};
    }
    const auto pixel_format = avif_aom_detail::avif_pixel_format_from_chroma(
        applied_chroma);
    if (!pixel_format) {
      return std::unexpected{pixel_format.error()};
    }

    avif_aom_detail::AvifImage avif_image{avifImageCreate(
        static_cast<std::uint32_t>(image.width),
        static_cast<std::uint32_t>(image.height), bit_depth, *pixel_format)};
    if (!avif_image) {
      return std::unexpected{"无法创建 libavif image。"};
    }
    avif_image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    avif_image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    avif_image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
    avif_image->yuvRange = AVIF_RANGE_FULL;

    auto rgb = avif_aom_detail::rgb_source_for_encode(image, **plane, avif_image.get(), bit_depth);
    if (!rgb) {
      return std::unexpected{rgb.error()};
    }
    auto result = avifImageRGBToYUV(avif_image.get(), &rgb->rgb);
    if (result != AVIF_RESULT_OK) {
      return std::unexpected{std::format("AVIF RGB 转 YUV 失败: {}",
                                         avifResultToString(result))};
    }

    avif_aom_detail::AvifEncoder encoder{avifEncoderCreate()};
    if (!encoder) {
      return std::unexpected{"无法创建 libavif encoder。"};
    }
    encoder->codecChoice = avif_aom_detail::codec_choice_for(actual_mode);
    encoder->quality = std::clamp(settings.quality, 1, 100);
    encoder->qualityAlpha = 100;
    encoder->speed = std::clamp(settings.speed, 0, 10);
    encoder->maxThreads = std::max(1, settings.resources.encoder_threads_per_file);

    avif_aom_detail::AvifRwData output{new avifRWData{}};
    output->data = nullptr;
    output->size = 0;
    result = avifEncoderWrite(encoder.get(), avif_image.get(), output.get());
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

    return NativeEncodeResult{.encoded = std::move(encoded),
                              .diagnostics = EncodeDiagnostics{
                                  .encoder_id = avif_encoder_mode_name(actual_mode),
                                  .requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder),
                                  .requested_chroma = chroma_mode_name(settings.requested_chroma_mode),
                                  .applied_chroma = chroma_mode_name(applied_chroma),
                                  .requested_bit_depth = settings.requested_bit_depth,
                                  .applied_bit_depth = bit_depth,
                                  .bit_depth_reason = settings.bit_depth_reason.empty()
                                                          ? (settings.requested_bit_depth ? "explicit bit-depth requested"
                                                                                          : "auto selected encoder default bit-depth")
                                                          : settings.bit_depth_reason,
                                  .fallback_reason = settings.encoder_fallback_reason,
                                  .encoder_license = actual_mode == AvifEncoderMode::svt
                                                         ? "BSD-3-Clause"
                                                         : "BSD-2-Clause",
                                  .speed_mapping = avif_aom_detail::libavif_speed_mapping(actual_mode, settings.speed),
                                  .encoder_threads = settings.resources.encoder_threads_per_file,
                                  .memory_budget_bytes = settings.resources.memory_limit_bytes},
                              .final_quality = std::clamp(settings.quality, 1, 100),
                              .lossless = settings.quality >= 100,
                              .search_attempt_count = 1};
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
    auto plane = avif_aom_detail::rgba8_plane(image, "zenravif");
    if (!plane) {
      return std::unexpected{plane.error()};
    }
    const int bit_depth = settings.bit_depth.value_or(8);
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
      return std::unexpected{"zenravif encoder 只支持 8、10、12-bit 输出。"};
    }
    const auto applied_chroma = avif_aom_detail::applied_chroma_from_settings(
        settings.chroma_mode);
    if (applied_chroma != ChromaMode::yuv420 && applied_chroma != ChromaMode::yuv444) {
      return std::unexpected{"zenravif encoder only supports 420 or 444 chroma."};
    }

    avif_aom_detail::ZenravifBytes output{};
    std::array<std::uint8_t, 512> error{};
    const int speed = std::clamp(settings.speed, 1, 10);
    const int code = zenravif_bridge_encode_rgba8(
        reinterpret_cast<const std::uint8_t*>((*plane)->bytes.data()),
        image.width, image.height, (*plane)->stride,
        std::clamp(settings.quality, 1, 100), speed, bit_depth,
        avif_aom_detail::chroma_numeric(applied_chroma),
        static_cast<std::size_t>(std::max(1, settings.resources.encoder_threads_per_file)),
        &output.output, error.data(), error.size());
    if (code != 0) {
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

    return NativeEncodeResult{.encoded = std::move(encoded),
                              .diagnostics = EncodeDiagnostics{
                                  .encoder_id = "zenrav1e",
                                  .requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder),
                                  .requested_chroma = chroma_mode_name(settings.requested_chroma_mode),
                                  .applied_chroma = chroma_mode_name(applied_chroma),
                                  .requested_bit_depth = settings.requested_bit_depth,
                                  .applied_bit_depth = bit_depth,
                                  .bit_depth_reason = settings.bit_depth_reason.empty()
                                                          ? (settings.requested_bit_depth ? "explicit bit-depth requested"
                                                                                          : "auto selected encoder default bit-depth")
                                                          : settings.bit_depth_reason,
                                  .fallback_reason = settings.encoder_fallback_reason,
                                  .encoder_experimental = true,
                                  .encoder_license = "AGPL-3.0-only OR LicenseRef-Imazen-Commercial",
                                  .speed_mapping = SpeedMapping{.user_speed = speed,
                                                                .codec_value = speed,
                                                                .codec_key = "zenravif:speed"},
                                  .encoder_threads = settings.resources.encoder_threads_per_file,
                                  .memory_budget_bytes = settings.resources.memory_limit_bytes},
                              .final_quality = std::clamp(settings.quality, 1, 100),
                              .lossless = settings.quality >= 100,
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

    const auto byte_count = static_cast<std::size_t>(rgb.rowBytes) * rgb.height;
    ImagePlane plane{.stride = rgb.rowBytes};
    plane.bytes.resize(byte_count);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(rgb.pixels), byte_count,
                        plane.bytes.begin());
    ImageBuffer out{.width = rgb.width,
                    .height = rgb.height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = image->alphaPlane != nullptr ? AlphaMode::straight : AlphaMode::none,
                    .bit_depth = 8};
    out.planes.push_back(std::move(plane));
    return ImageDecodeResult{.image = std::move(out), .decoder_id = "libavif"};
  }
};

}  // namespace awj
