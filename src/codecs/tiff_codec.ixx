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

#include <tiffio.h>

export module awj.tiff_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace tiff_detail {

struct TiffDeleter {
  void operator()(TIFF* value) const noexcept {
    if (value != nullptr) {
      TIFFClose(value);
    }
  }
};

using TiffPtr = std::unique_ptr<TIFF, TiffDeleter>;

}  // namespace tiff_detail

export class TiffImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libtiff"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".tif", L".tiff"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    const auto narrow_path = path.string();
    tiff_detail::TiffPtr tiff{TIFFOpen(narrow_path.c_str(), "r")};
    if (!tiff) {
      return std::unexpected{std::format("打开 TIFF 失败: {}", path_to_utf8(path))};
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) != 1) {
      return std::unexpected{std::format("TIFF 尺寸无效: {}", path_to_utf8(path))};
    }
    return decoder_common::make_image_dimensions_checked(width, height, "TIFF");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    const auto narrow_path = path.string();
    tiff_detail::TiffPtr tiff{TIFFOpen(narrow_path.c_str(), "r")};
    if (!tiff) {
      return std::unexpected{std::format("打开 TIFF 失败: {}", path_to_utf8(path))};
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) != 1 ||
        width == 0 || height == 0) {
      return std::unexpected{std::format("TIFF 尺寸无效: {}", path_to_utf8(path))};
    }

    const auto stride = decoder_common::checked_rgba_stride(width, "TIFF decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "TIFF decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }

    std::vector<std::uint32_t> raster;
    const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    raster.resize(pixel_count);
    if (TIFFReadRGBAImageOriented(tiff.get(), width, height, raster.data(), ORIENTATION_TOPLEFT, 0) == 0) {
      return std::unexpected{std::format("TIFF 解码失败: {}", path_to_utf8(path))};
    }

    std::vector<std::byte> rgba(*byte_count);
    for (std::size_t i = 0; i < raster.size(); ++i) {
      const auto pixel = raster[i];
      rgba[i * 4 + 0] = static_cast<std::byte>(TIFFGetR(pixel));
      rgba[i * 4 + 1] = static_cast<std::byte>(TIFFGetG(pixel));
      rgba[i * 4 + 2] = static_cast<std::byte>(TIFFGetB(pixel));
      rgba[i * 4 + 3] = static_cast<std::byte>(TIFFGetA(pixel));
    }

    auto image = decoder_common::make_rgba_image(width, height, std::move(rgba),
                                                 AlphaMode::straight, "TIFF decoder");
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libtiff"};
  }
};

}  // namespace awj
