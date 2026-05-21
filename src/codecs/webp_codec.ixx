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
#include <vector>

#include <webp/decode.h>
#include <webp/encode.h>

export module awj.webp_codec;

import awj.codec;
import awj.config;
import awj.image;

export namespace awj {

namespace webp_detail {

struct WebPFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      WebPFree(value);
    }
  }
};

using WebPBytes = std::unique_ptr<std::uint8_t, WebPFreeDeleter>;

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 WebP 文件: {}", path.string())};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0) {
    return std::unexpected{std::format("WebP 文件为空: {}", path.string())};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) {
    return std::unexpected{std::format("读取 WebP 文件失败: {}", path.string())};
  }
  return bytes;
}

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{"WebP encoder 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = image.width * 4;
  if (plane.stride < expected_stride ||
      plane.bytes.size() < plane.stride * image.height) {
    return std::unexpected{"WebP encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
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
      return std::unexpected{std::format("WebP 文件信息无效: {}", path.string())};
    }

    webp_detail::WebPBytes decoded{WebPDecodeRGBA(data, bytes->size(), &width, &height)};
    if (!decoded) {
      return std::unexpected{std::format("WebP 解码失败: {}", path.string())};
    }

    const auto stride = static_cast<std::size_t>(width) * 4;
    const auto byte_count = stride * static_cast<std::size_t>(height);
    ImagePlane plane{.stride = stride};
    plane.bytes.resize(byte_count);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(decoded.get()), byte_count,
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
    std::uint8_t* raw_output = nullptr;
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    const int stride = static_cast<int>((*plane)->stride);
    const bool lossless = settings.visual_quality == 100 || settings.quality >= 100;
    const auto output_size = lossless
                                 ? WebPEncodeLosslessRGBA(rgba, width, height,
                                                          stride, &raw_output)
                                 : WebPEncodeRGBA(rgba, width, height, stride,
                                                  static_cast<float>(settings.quality),
                                                  &raw_output);
    webp_detail::WebPBytes output{raw_output};
    if (output_size == 0 || !output) {
      return std::unexpected{"WebP 编码失败。"};
    }

    EncodedImage encoded{.codec_name = "libwebp"};
    encoded.bytes.resize(output_size);
    std::ranges::copy_n(reinterpret_cast<std::byte*>(output.get()), output_size,
                        encoded.bytes.begin());

    return NativeEncodeResult{.encoded = std::move(encoded),
                              .diagnostics = EncodeDiagnostics{
                                  .encoder_id = "libwebp",
                                  .speed_mapping = map_webp_speed_to_method(settings.speed),
                                  .encoder_threads = settings.resources.encoder_threads_per_file,
                                  .memory_budget_bytes = settings.resources.memory_limit_bytes},
                              .final_quality = lossless ? 100 : settings.quality,
                              .lossless = lossless,
                              .search_attempt_count = 1};
  }
};

}  // namespace awj
