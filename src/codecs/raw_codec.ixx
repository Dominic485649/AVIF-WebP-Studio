module;

#include <expected>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

export module awj.raw_codec;

import awj.codec;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;
import awj.raw_image_io;

export namespace awj {

export class RawImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "awj-raw"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".awsraw"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto decoded = read_raw_image_file(path);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }
    if (decoded->width > std::numeric_limits<std::uint32_t>::max() ||
        decoded->height > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected{"awj-raw 尺寸无效。"};
    }
    return decoder_common::make_image_dimensions_checked(static_cast<std::uint32_t>(decoded->width),
                                                         static_cast<std::uint32_t>(decoded->height),
                                                         "awj-raw");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto image = read_raw_image_file(path);
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "awj-raw"};
  }
};

}  // namespace awj
