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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.decoder_common;

import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace decoder_common {

std::wstring lower_extension(const fs::path& path) {
  auto ext = path.extension().wstring();
  std::ranges::transform(ext, ext.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return ext;
}

bool extension_is_one_of(const fs::path& path, std::span<const std::wstring_view> extensions) {
  const auto ext = lower_extension(path);
  return std::ranges::any_of(extensions, [&](std::wstring_view value) { return ext == value; });
}

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path,
    std::string_view codec_name) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 {} 文件: {}", codec_name, path_to_utf8(path))};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0) {
    return std::unexpected{std::format("{} 文件为空: {}", codec_name, path_to_utf8(path))};
  }
  const auto file_size = static_cast<std::uint64_t>(size);
  if (file_size > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{std::format(
        "{} 文件超过 20 GiB 输入上限: {}", codec_name, path_to_utf8(path))};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) {
    return std::unexpected{std::format("读取 {} 文件失败: {}", codec_name, path_to_utf8(path))};
  }
  return bytes;
}

std::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context) {
  if (width == 0 || width > std::numeric_limits<std::size_t>::max() / 4) {
    return std::unexpected{std::format("{} 输入宽度无效。", context)};
  }
  return width * 4;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height == 0 || height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  return stride * height;
}

std::expected<ImageBuffer, std::string> make_rgba_image(std::size_t width,
                                                        std::size_t height,
                                                        std::vector<std::byte> rgba,
                                                        AlphaMode alpha_mode,
                                                        std::string_view context,
                                                        std::optional<ImageSourceInfo> source_info = {}) {
  const auto stride = checked_rgba_stride(width, context);
  if (!stride) {
    return std::unexpected{stride.error()};
  }
  const auto byte_count = checked_image_bytes(*stride, height, context);
  if (!byte_count) {
    return std::unexpected{byte_count.error()};
  }
  if (rgba.size() < *byte_count) {
    return std::unexpected{std::format("{} RGBA buffer 尺寸无效。", context)};
  }
  rgba.resize(*byte_count);
  ImagePlane plane{.bytes = std::move(rgba), .stride = *stride};
  ImageBuffer image{.width = width,
                    .height = height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = alpha_mode,
                    .bit_depth = 8,
                    .source_info = std::move(source_info)};
  image.planes.push_back(std::move(plane));
  return image;
}

std::expected<bool, std::string> has_non_opaque_alpha(const ImageBuffer& image,
                                                       std::string_view context) {
  if (image.alpha_mode == AlphaMode::none) {
    return false;
  }
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 || image.planes.empty()) {
    return std::unexpected{std::format("{} alpha 检测需要 8-bit RGBA ImageBuffer。", context)};
  }
  const auto stride = checked_rgba_stride(image.width, context);
  if (!stride) {
    return std::unexpected{stride.error()};
  }
  const auto byte_count = checked_image_bytes(image.planes.front().stride, image.height, context);
  if (!byte_count) {
    return std::unexpected{byte_count.error()};
  }
  const auto& plane = image.planes.front();
  if (plane.stride < *stride || plane.bytes.size() < *byte_count) {
    return std::unexpected{std::format("{} RGBA buffer 尺寸无效。", context)};
  }
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(plane.bytes.data() + y * plane.stride);
    for (std::size_t x = 0; x < image.width; ++x) {
      if (row[x * 4 + 3] != 255) {
        return true;
      }
    }
  }
  return false;
}

std::expected<ImageDimensions, std::string> make_image_dimensions_checked(std::uint64_t width,
                                                                            std::uint64_t height,
                                                                            std::string_view context) {
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{std::format("{} 尺寸无效。", context)};
  }
  if (width > std::numeric_limits<std::uint64_t>::max() / height) {
    return std::unexpected{std::format("{} 尺寸过大。", context)};
  }
  if (width * height > encoding_defaults::max_decoded_pixels) {
    return std::unexpected{std::format("{} 像素数超过当前解码上限。", context)};
  }
  return make_image_dimensions(static_cast<std::uint32_t>(width),
                               static_cast<std::uint32_t>(height));
}

}  // namespace decoder_common

}  // namespace awj
