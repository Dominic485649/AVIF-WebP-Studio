module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <string>
#include <vector>

export module awj.raw_image_io;

import awj.image;

export namespace awj {

namespace raw_image_detail {

inline constexpr char magic[] = {'A', 'W', 'S', 'R', 'A', 'W', '1', '\0'};
inline constexpr std::uint32_t pixel_format_rgba = 1;
inline constexpr std::uint32_t alpha_none = 0;
inline constexpr std::uint32_t alpha_straight = 1;
inline constexpr std::uint32_t alpha_premultiplied = 2;

struct Header {
  char magic[8]{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t pixel_format{};
  std::uint32_t alpha_mode{};
  std::uint32_t bit_depth{};
  std::uint32_t stride{};
  std::uint64_t byte_count{};
};

std::uint32_t alpha_to_raw(AlphaMode mode) noexcept {
  switch (mode) {
    case AlphaMode::straight:
      return alpha_straight;
    case AlphaMode::premultiplied:
      return alpha_premultiplied;
    case AlphaMode::none:
    default:
      return alpha_none;
  }
}

AlphaMode alpha_from_raw(std::uint32_t value) noexcept {
  switch (value) {
    case alpha_straight:
      return AlphaMode::straight;
    case alpha_premultiplied:
      return AlphaMode::premultiplied;
    case alpha_none:
    default:
      return AlphaMode::none;
  }
}

}  // namespace raw_image_detail

export std::expected<void, std::string> write_raw_image_file(
    const fs::path& path,
    const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.planes.empty()) {
    return std::unexpected{"raw image writer requires RGBA ImageBuffer."};
  }
  const auto& plane = image.planes.front();
  if (image.width == 0 || image.height == 0 || plane.stride == 0 ||
      plane.bytes.empty()) {
    return std::unexpected{"raw image writer received an empty image."};
  }
  if (image.width > std::numeric_limits<std::uint32_t>::max() ||
      image.height > std::numeric_limits<std::uint32_t>::max() ||
      plane.stride > std::numeric_limits<std::uint32_t>::max() ||
      plane.stride > std::numeric_limits<std::uint64_t>::max() / image.height) {
    return std::unexpected{"raw image dimensions exceed file format limits."};
  }
  const auto min_bytes = static_cast<std::uint64_t>(plane.stride) * image.height;
  if (plane.bytes.size() < min_bytes) {
    return std::unexpected{"raw image payload is smaller than its dimensions."};
  }
  raw_image_detail::Header header{};
  std::ranges::copy(raw_image_detail::magic, header.magic);
  header.width = static_cast<std::uint32_t>(image.width);
  header.height = static_cast<std::uint32_t>(image.height);
  header.pixel_format = raw_image_detail::pixel_format_rgba;
  header.alpha_mode = raw_image_detail::alpha_to_raw(image.alpha_mode);
  header.bit_depth = static_cast<std::uint32_t>(image.bit_depth);
  header.stride = static_cast<std::uint32_t>(plane.stride);
  header.byte_count = static_cast<std::uint64_t>(plane.bytes.size());

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    return std::unexpected{std::format("cannot create raw image directory: {}", ec.message())};
  }
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return std::unexpected{std::format("cannot write raw image file: {}", path.string())};
  }
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  output.write(reinterpret_cast<const char*>(plane.bytes.data()),
               static_cast<std::streamsize>(plane.bytes.size()));
  if (!output) {
    return std::unexpected{std::format("failed writing raw image file: {}", path.string())};
  }
  return {};
}

export std::expected<ImageBuffer, std::string> read_raw_image_file(const fs::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("cannot read raw image file: {}", path.string())};
  }
  raw_image_detail::Header header{};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!input || std::memcmp(header.magic, raw_image_detail::magic, sizeof(header.magic)) != 0) {
    return std::unexpected{"raw image header is invalid."};
  }
  if (header.pixel_format != raw_image_detail::pixel_format_rgba ||
      (header.bit_depth != 8 && header.bit_depth != 10 && header.bit_depth != 12 &&
       header.bit_depth != 16)) {
    return std::unexpected{"raw image format is unsupported."};
  }
  if (header.width == 0 || header.height == 0 || header.stride == 0 ||
      header.width > std::numeric_limits<std::uint64_t>::max() / 4ull /
                         (header.bit_depth > 8 ? 2ull : 1ull)) {
    return std::unexpected{"raw image dimensions are invalid."};
  }
  const auto expected_min_stride = static_cast<std::uint64_t>(header.width) * 4ull *
                                   (header.bit_depth > 8 ? 2ull : 1ull);
  if (header.width == 0 || header.height == 0 || header.stride < expected_min_stride ||
      header.height > std::numeric_limits<std::uint64_t>::max() / header.stride) {
    return std::unexpected{"raw image dimensions are invalid."};
  }
  const auto min_bytes = static_cast<std::uint64_t>(header.stride) * header.height;
  if (header.byte_count < min_bytes || header.byte_count != min_bytes ||
      header.byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected{"raw image byte count is invalid."};
  }
  ImagePlane plane{.stride = header.stride};
  plane.bytes.resize(static_cast<std::size_t>(header.byte_count));
  input.read(reinterpret_cast<char*>(plane.bytes.data()),
             static_cast<std::streamsize>(plane.bytes.size()));
  if (!input) {
    return std::unexpected{"failed reading raw image payload."};
  }
  ImageBuffer image{.width = header.width,
                    .height = header.height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = raw_image_detail::alpha_from_raw(header.alpha_mode),
                    .bit_depth = static_cast<int>(header.bit_depth)};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace awj
