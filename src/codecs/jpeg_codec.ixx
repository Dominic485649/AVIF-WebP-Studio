module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <setjmp.h>
#include <string>
#include <string_view>
#include <vector>

#include <jpeglib.h>

export module awj.jpeg_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace jpeg_detail {

struct ErrorManager {
  jpeg_error_mgr pub{};
  jmp_buf jump{};
  char message[JMSG_LENGTH_MAX]{};
};

void error_exit(j_common_ptr cinfo) {
  auto* error = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, error->message);
  longjmp(error->jump, 1);
}

struct DecompressCleanup {
  jpeg_decompress_struct* cinfo{};
  bool created{};

  ~DecompressCleanup() {
    if (created && cinfo != nullptr) {
      jpeg_destroy_decompress(cinfo);
    }
  }
};

}  // namespace jpeg_detail

export class JpegImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libjpeg-turbo"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".jpg", L".jpeg", L".jpe", L".jfif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "JPEG");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }

    jpeg_decompress_struct cinfo{};
    jpeg_detail::ErrorManager error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_detail::error_exit;
    jpeg_detail::DecompressCleanup cleanup{.cinfo = &cinfo};
    if (setjmp(error.jump) != 0) {
      return std::unexpected{std::format("JPEG 读取尺寸失败: {}", error.message)};
    }
    if (bytes->size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
      return std::unexpected{"JPEG 文件超过 libjpeg API 限制。"};
    }
    jpeg_create_decompress(&cinfo);
    cleanup.created = true;
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes->data()),
                 static_cast<unsigned long>(bytes->size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
      return std::unexpected{std::format("JPEG 文件头无效: {}", path_to_utf8(path))};
    }
    return decoder_common::make_image_dimensions_checked(cinfo.image_width,
                                                         cinfo.image_height,
                                                         "JPEG");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "JPEG");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }

    jpeg_decompress_struct cinfo{};
    jpeg_detail::ErrorManager error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_detail::error_exit;
    jpeg_detail::DecompressCleanup cleanup{.cinfo = &cinfo};
    if (setjmp(error.jump) != 0) {
      return std::unexpected{std::format("JPEG 解码失败: {}", error.message)};
    }

    if (bytes->size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
      return std::unexpected{"JPEG 文件超过 libjpeg API 限制。"};
    }
    jpeg_create_decompress(&cinfo);
    cleanup.created = true;
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes->data()),
                 static_cast<unsigned long>(bytes->size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
      return std::unexpected{std::format("JPEG 文件头无效: {}", path_to_utf8(path))};
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);
    const auto width = static_cast<std::size_t>(cinfo.output_width);
    const auto height = static_cast<std::size_t>(cinfo.output_height);
    const auto stride = decoder_common::checked_rgba_stride(width, "JPEG decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "JPEG decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    std::vector<std::byte> rgba(*byte_count);
    while (cinfo.output_scanline < cinfo.output_height) {
      auto* row = reinterpret_cast<JSAMPROW>(rgba.data() + cinfo.output_scanline * *stride);
      jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);

    auto image = decoder_common::make_rgba_image(width, height, std::move(rgba),
                                                 AlphaMode::none, "JPEG decoder");
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libjpeg-turbo"};
  }
};

}  // namespace awj
