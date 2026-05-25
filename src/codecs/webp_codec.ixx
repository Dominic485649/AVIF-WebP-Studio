module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>

export module awj.webp_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace webp_detail {

struct WebPFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      WebPFree(value);
    }
  }
};

struct WebPMemoryWriterGuard {
  WebPMemoryWriter writer{};
  WebPMemoryWriterGuard() { WebPMemoryWriterInit(&writer); }
  ~WebPMemoryWriterGuard() { WebPMemoryWriterClear(&writer); }
  WebPMemoryWriterGuard(const WebPMemoryWriterGuard&) = delete;
  WebPMemoryWriterGuard& operator=(const WebPMemoryWriterGuard&) = delete;
};

struct WebPPictureGuard {
  WebPPicture picture{};
  WebPPictureGuard() { WebPPictureInit(&picture); }
  ~WebPPictureGuard() { WebPPictureFree(&picture); }
  WebPPictureGuard(const WebPPictureGuard&) = delete;
  WebPPictureGuard& operator=(const WebPPictureGuard&) = delete;
};

struct WebPDataGuard {
  WebPDataGuard() = default;
  WebPData data{};
  ~WebPDataGuard() { WebPDataClear(&data); }
  WebPDataGuard(const WebPDataGuard&) = delete;
  WebPDataGuard& operator=(const WebPDataGuard&) = delete;
};

using WebPBytes = std::unique_ptr<std::uint8_t, WebPFreeDeleter>;

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "WebP");
}

std::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context) {
  if (width > std::numeric_limits<std::size_t>::max() / 4) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  return width * 4;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  return stride * height;
}

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{"WebP encoder 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = checked_rgba_stride(image.width, "WebP encoder");
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, "WebP encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.stride < *expected_stride || plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"WebP encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

const MetadataBlock* first_icc_metadata(const ImageBuffer& image) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == MetadataKind::icc && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

std::expected<std::vector<std::byte>, std::string> mux_icc(
    std::span<const std::byte> encoded,
    const MetadataBlock& icc) {
  WebPData image_data{.bytes = reinterpret_cast<const std::uint8_t*>(encoded.data()),
                      .size = encoded.size()};
  WebPData icc_data{.bytes = reinterpret_cast<const std::uint8_t*>(icc.bytes.data()),
                    .size = icc.bytes.size()};
  WebPDataGuard assembled{};
  WebPMux* mux = WebPMuxNew();
  if (mux == nullptr) {
    return std::unexpected{"无法创建 WebP mux。"};
  }
  const auto mux_guard = std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)>{mux, &WebPMuxDelete};
  if (WebPMuxSetImage(mux, &image_data, 1) != WEBP_MUX_OK) {
    return std::unexpected{"WebP mux 设置图像失败。"};
  }
  if (WebPMuxSetChunk(mux, "ICCP", &icc_data, 1) != WEBP_MUX_OK) {
    return std::unexpected{"WebP mux 设置 ICC 失败。"};
  }
  if (WebPMuxAssemble(mux, &assembled.data) != WEBP_MUX_OK ||
      assembled.data.bytes == nullptr || assembled.data.size == 0) {
    return std::unexpected{"WebP mux 输出失败。"};
  }
  std::vector<std::byte> bytes(assembled.data.size);
  std::ranges::copy_n(reinterpret_cast<const std::byte*>(assembled.data.bytes),
                      assembled.data.size, bytes.begin());
  return bytes;
}

}  // namespace webp_detail

export class WebPImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libwebp"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".webp";
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = webp_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    int width = 0;
    int height = 0;
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
    if (WebPGetInfo(data, bytes->size(), &width, &height) == 0 ||
        width <= 0 || height <= 0) {
      return std::unexpected{std::format("WebP 文件信息无效: {}", path_to_utf8(path))};
    }
    return decoder_common::make_image_dimensions_checked(static_cast<std::uint32_t>(width),
                                                         static_cast<std::uint32_t>(height),
                                                         "WebP");
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    auto bytes = webp_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }

    int width = 0;
    int height = 0;
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
    if (WebPGetInfo(data, bytes->size(), &width, &height) == 0 ||
        width <= 0 || height <= 0) {
      return std::unexpected{std::format("WebP 文件信息无效: {}", path_to_utf8(path))};
    }

    webp_detail::WebPBytes decoded{WebPDecodeRGBA(data, bytes->size(), &width, &height)};
    if (!decoded) {
      return std::unexpected{std::format("WebP 解码失败: {}", path_to_utf8(path))};
    }

    const auto stride = webp_detail::checked_rgba_stride(static_cast<std::size_t>(width), "WebP decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = webp_detail::checked_image_bytes(*stride, static_cast<std::size_t>(height), "WebP decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    ImagePlane plane{.stride = *stride};
    plane.bytes.resize(*byte_count);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(decoded.get()), *byte_count,
                        plane.bytes.begin());

    ImageBuffer image{.width = static_cast<std::size_t>(width),
                      .height = static_cast<std::size_t>(height),
                      .pixel_format = PixelFormat::rgba,
                      .alpha_mode = AlphaMode::straight,
                      .bit_depth = 8};
    image.planes.push_back(std::move(plane));
    return ImageDecodeResult{.image = std::move(image), .decoder_id = "libwebp"};
  }
};

export class WebPImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libwebp"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::webp,
                             .features = CodecFeature::lossless |
                                         CodecFeature::alpha |
                                         CodecFeature::visual_quality_search,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
    auto plane = webp_detail::rgba_plane(image);
    if (!plane) {
      return std::unexpected{plane.error()};
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        (*plane)->stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return std::unexpected{"WebP encoder 输入尺寸超过 libwebp API 限制。"};
    }

    const auto* rgba = reinterpret_cast<const std::uint8_t*>((*plane)->bytes.data());
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    const int stride = static_cast<int>((*plane)->stride);
    const bool lossless = settings.visual_quality == 100 || settings.quality >= 100;

    WebPConfig config{};
    if (WebPConfigInit(&config) == 0) {
      return std::unexpected{"WebP config 初始化失败。"};
    }
    const int method = settings.speed_explicit ? map_webp_speed_to_method(settings.speed).codec_value
                                               : 0;
    config.quality = static_cast<float>(lossless ? 100 : std::clamp(settings.quality, 1, 100));
    config.lossless = lossless ? 1 : 0;
    config.method = method;
    config.alpha_quality = 100;
    config.thread_level = settings.resources.encoder_threads_per_file > 1 ? 1 : 0;
    if (WebPValidateConfig(&config) == 0) {
      return std::unexpected{"WebP config 参数无效。"};
    }

    webp_detail::WebPPictureGuard picture{};
    picture.picture.use_argb = 1;
    picture.picture.width = width;
    picture.picture.height = height;
    webp_detail::WebPMemoryWriterGuard writer{};
    picture.picture.writer = WebPMemoryWrite;
    picture.picture.custom_ptr = &writer.writer;
    if (WebPPictureImportRGBA(&picture.picture, rgba, stride) == 0) {
      return std::unexpected{"WebP picture 导入 RGBA 失败。"};
    }
    if (WebPEncode(&config, &picture.picture) == 0) {
      return std::unexpected{std::format("WebP 编码失败，错误码 {}。", static_cast<int>(picture.picture.error_code))};
    }

    EncodedImage encoded{.codec_name = "libwebp"};
    encoded.bytes.resize(writer.writer.size);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(writer.writer.mem), writer.writer.size,
                        encoded.bytes.begin());
    if (!settings.strip_metadata) {
      if (const auto* icc = webp_detail::first_icc_metadata(image); icc != nullptr) {
        auto muxed = webp_detail::mux_icc(encoded.bytes, *icc);
        if (!muxed) {
          return std::unexpected{muxed.error()};
        }
        encoded.bytes = std::move(*muxed);
      }
    }

    return NativeEncodeResult{.encoded = std::move(encoded),
                              .diagnostics = EncodeDiagnostics{
                                  .encoder_id = "libwebp",
                                  .speed_mapping = SpeedMapping{.user_speed = settings.speed,
                                                                .codec_value = method,
                                                                .codec_key = "webp:method"},
                                  .encoder_threads = settings.resources.encoder_threads_per_file,
                                  .memory_budget_bytes = settings.resources.memory_limit_bytes},
                              .final_quality = lossless ? 100 : settings.quality,
                              .lossless = lossless,
                              .search_attempt_count = 1};
  }
};

}  // namespace awj
