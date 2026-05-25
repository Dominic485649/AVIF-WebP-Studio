module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <png.h>

export module awj.png_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace png_detail {

struct ReadState {
  const std::byte* data{};
  std::size_t size{};
  std::size_t offset{};
};

struct PngReadDeleter {
  void operator()(png_structp value) const noexcept {
    if (value != nullptr) {
      png_destroy_read_struct(&value, nullptr, nullptr);
    }
  }
};

struct PngInfoDeleter {
  png_structp png{};
  void operator()(png_infop value) const noexcept {
    if (png != nullptr && value != nullptr) {
      png_destroy_info_struct(png, &value);
    }
  }
};

using PngReadPtr = std::unique_ptr<png_struct, PngReadDeleter>;
using PngInfoPtr = std::unique_ptr<png_info, PngInfoDeleter>;

void read_callback(png_structp png, png_bytep out, png_size_t count) {
  auto* state = static_cast<ReadState*>(png_get_io_ptr(png));
  if (state == nullptr || count > state->size - state->offset) {
    png_error(png, "PNG input is truncated");
    return;
  }
  std::ranges::copy_n(reinterpret_cast<const png_byte*>(state->data + state->offset),
                      count, out);
  state->offset += count;
}

PixelFormat source_pixel_format_for_png(int color_type) noexcept {
  switch (color_type) {
    case PNG_COLOR_TYPE_GRAY:
      return PixelFormat::gray;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      return PixelFormat::rgba;
    case PNG_COLOR_TYPE_RGB:
      return PixelFormat::rgb;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      return PixelFormat::rgba;
    case PNG_COLOR_TYPE_PALETTE:
      return PixelFormat::rgb;
    default:
      return PixelFormat::unknown;
  }
}

}  // namespace png_detail

export class PngImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libpng"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".png"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "PNG");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    if (bytes->size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0) {
      return std::unexpected{std::format("PNG 签名无效: {}", path_to_utf8(path))};
    }
    png_detail::PngReadPtr png{png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr)};
    if (!png) {
      return std::unexpected{"创建 PNG decoder 失败。"};
    }
    png_detail::PngInfoPtr info{png_create_info_struct(png.get()), png_detail::PngInfoDeleter{.png = png.get()}};
    if (!info) {
      return std::unexpected{"创建 PNG info 失败。"};
    }
    png_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
    png_set_read_fn(png.get(), &state, png_detail::read_callback);
    if (setjmp(png_jmpbuf(png.get())) != 0) {
      return std::unexpected{std::format("PNG 读取尺寸失败: {}", path_to_utf8(path))};
    }
    png_read_info(png.get(), info.get());
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png.get(), info.get(), &width, &height, &bit_depth, &color_type,
                 nullptr, nullptr, nullptr);
    return decoder_common::make_image_dimensions_checked(width, height, "PNG");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "PNG");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    if (bytes->size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0) {
      return std::unexpected{std::format("PNG 签名无效: {}", path_to_utf8(path))};
    }

    png_detail::PngReadPtr png{png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr)};
    if (!png) {
      return std::unexpected{"创建 PNG decoder 失败。"};
    }
    png_detail::PngInfoPtr info{png_create_info_struct(png.get()), png_detail::PngInfoDeleter{.png = png.get()}};
    if (!info) {
      return std::unexpected{"创建 PNG info 失败。"};
    }

    png_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
    png_set_read_fn(png.get(), &state, png_detail::read_callback);

    if (setjmp(png_jmpbuf(png.get())) != 0) {
      return std::unexpected{std::format("PNG 解码失败: {}", path_to_utf8(path))};
    }

    png_read_info(png.get(), info.get());
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png.get(), info.get(), &width, &height, &bit_depth, &color_type,
                 nullptr, nullptr, nullptr);
    if (width == 0 || height == 0) {
      return std::unexpected{std::format("PNG 尺寸无效: {}", path_to_utf8(path))};
    }

    if (bit_depth == 16) {
      png_set_strip_16(png.get());
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_palette_to_rgb(png.get());
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
      png_set_expand_gray_1_2_4_to_8(png.get());
    }
    if (png_get_valid(png.get(), info.get(), PNG_INFO_tRNS)) {
      png_set_tRNS_to_alpha(png.get());
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
      png_set_gray_to_rgb(png.get());
    }
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png.get(), info.get(), PNG_INFO_tRNS)) {
      png_set_filler(png.get(), 0xff, PNG_FILLER_AFTER);
    }
    png_set_interlace_handling(png.get());
    png_read_update_info(png.get(), info.get());

    const auto stride = decoder_common::checked_rgba_stride(width, "PNG decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "PNG decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    std::vector<std::byte> rgba(*byte_count);
    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; ++y) {
      rows[y] = reinterpret_cast<png_bytep>(rgba.data() + y * *stride);
    }
    png_read_image(png.get(), rows.data());
    png_read_end(png.get(), nullptr);

    auto image = decoder_common::make_rgba_image(
        width, height, std::move(rgba), AlphaMode::straight, "PNG decoder",
        ImageSourceInfo{.pixel_format = png_detail::source_pixel_format_for_png(color_type),
                        .bit_depth = bit_depth});
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libpng"};
  }
};

}  // namespace awj
