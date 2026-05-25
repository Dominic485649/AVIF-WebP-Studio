module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <libraw/libraw.h>

export module awj.libraw_codec;

import awj.codec;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace libraw_detail {

struct ProcessedImageCleanup {
  libraw_processed_image_t* image{};
  ~ProcessedImageCleanup() {
    if (image != nullptr) {
      LibRaw::dcraw_clear_mem(image);
    }
  }
};

}  // namespace libraw_detail

export class LibRawImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libraw"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {
        L".dng", L".cr2", L".cr3", L".nef", L".arw", L".rw2", L".orf",
        L".raf", L".pef", L".srw", L".x3f", L".3fr", L".erf", L".kdc",
        L".mrw", L".raw"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    LibRaw raw;
    const auto opened = raw.open_file(path.string().c_str());
    if (opened != LIBRAW_SUCCESS) {
      return std::unexpected{std::format("LibRaw 打开失败: {}", libraw_strerror(opened))};
    }
    const auto& sizes = raw.imgdata.sizes;
    const auto width = sizes.width > 0 ? sizes.width : sizes.raw_width;
    const auto height = sizes.height > 0 ? sizes.height : sizes.raw_height;
    return decoder_common::make_image_dimensions_checked(static_cast<std::uint32_t>(width),
                                                         static_cast<std::uint32_t>(height),
                                                         "LibRaw");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    LibRaw raw;
    const auto opened = raw.open_file(path.string().c_str());
    if (opened != LIBRAW_SUCCESS) {
      return std::unexpected{std::format("LibRaw 打开失败: {}", libraw_strerror(opened))};
    }
    const auto unpacked = raw.unpack();
    if (unpacked != LIBRAW_SUCCESS) {
      return std::unexpected{std::format("LibRaw 解包失败: {}", libraw_strerror(unpacked))};
    }
    raw.imgdata.params.output_bps = 8;
    raw.imgdata.params.output_color = 1;
    raw.imgdata.params.no_auto_bright = 1;
    raw.imgdata.params.use_camera_wb = 1;
    const auto processed = raw.dcraw_process();
    if (processed != LIBRAW_SUCCESS) {
      return std::unexpected{std::format("LibRaw 处理失败: {}", libraw_strerror(processed))};
    }

    int process_warnings = 0;
    libraw_processed_image_t* processed_image = raw.dcraw_make_mem_image(&process_warnings);
    libraw_detail::ProcessedImageCleanup cleanup{.image = processed_image};
    if (processed_image == nullptr) {
      return std::unexpected{"LibRaw 未产生图像输出。"};
    }

    if (processed_image->type != LIBRAW_IMAGE_BITMAP || processed_image->colors < 3 || processed_image->bits != 8 ||
        processed_image->width <= 0 || processed_image->height <= 0) {
      return std::unexpected{"LibRaw 输出格式无效。"};
    }

    const auto width = static_cast<std::size_t>(processed_image->width);
    const auto height = static_cast<std::size_t>(processed_image->height);
    const auto stride = decoder_common::checked_rgba_stride(width, "LibRaw decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "LibRaw decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }

    std::vector<std::byte> rgba(*byte_count);
    const auto channels = static_cast<std::size_t>(processed_image->colors);
    if (height != 0 && width > std::numeric_limits<std::size_t>::max() / height) {
      return std::unexpected{"LibRaw 输出像素数溢出。"};
    }
    const auto pixel_count = width * height;
    if (channels != 0 && pixel_count > std::numeric_limits<std::size_t>::max() / channels) {
      return std::unexpected{"LibRaw 输出数据大小溢出。"};
    }
    const auto required_source_bytes = pixel_count * channels;
    if (processed_image->data_size < required_source_bytes) {
      return std::unexpected{"LibRaw 输出数据短于声明尺寸。"};
    }
    const auto* source = processed_image->data;
    for (std::size_t i = 0; i < pixel_count; ++i) {
      rgba[i * 4 + 0] = static_cast<std::byte>(source[i * channels + 0]);
      rgba[i * 4 + 1] = static_cast<std::byte>(source[i * channels + 1]);
      rgba[i * 4 + 2] = static_cast<std::byte>(source[i * channels + 2]);
      rgba[i * 4 + 3] = std::byte{0xff};
    }

    auto image = decoder_common::make_rgba_image(width, height, std::move(rgba),
                                                 AlphaMode::none, "LibRaw decoder");
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libraw"};
  }
};

}  // namespace awj
