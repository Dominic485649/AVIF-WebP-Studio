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

#include <gif_lib.h>

export module awj.gif_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace gif_detail {

struct GifDeleter {
  void operator()(GifFileType* value) const noexcept {
    if (value != nullptr) {
      int error = 0;
      DGifCloseFile(value, &error);
    }
  }
};

using GifPtr = std::unique_ptr<GifFileType, GifDeleter>;

struct ReadState {
  const std::byte* data{};
  std::size_t size{};
  std::size_t offset{};
};

int read_callback(GifFileType* gif, GifByteType* out, int count) {
  auto* state = static_cast<ReadState*>(gif->UserData);
  if (state == nullptr || count < 0) {
    return 0;
  }
  const auto requested = static_cast<std::size_t>(count);
  const auto available = state->size - state->offset;
  const auto copied = std::min(requested, available);
  std::ranges::copy_n(reinterpret_cast<const GifByteType*>(state->data + state->offset),
                      copied, out);
  state->offset += copied;
  return static_cast<int>(copied);
}

ColorMapObject* active_color_map(const GifFileType* gif, const SavedImage& image) noexcept {
  return image.ImageDesc.ColorMap != nullptr ? image.ImageDesc.ColorMap : gif->SColorMap;
}

}  // namespace gif_detail

export class GifImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "giflib"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".gif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "GIF");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    gif_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
    int error = 0;
    gif_detail::GifPtr gif{DGifOpen(&state, gif_detail::read_callback, &error)};
    if (!gif) {
      return std::unexpected{std::format("打开 GIF 失败，错误码: {}", error)};
    }
    if (gif->SWidth <= 0 || gif->SHeight <= 0) {
      return std::unexpected{std::format("GIF 尺寸无效: {}", path_to_utf8(path))};
    }
    return decoder_common::make_image_dimensions_checked(static_cast<std::uint32_t>(gif->SWidth),
                                                         static_cast<std::uint32_t>(gif->SHeight),
                                                         "GIF");
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "GIF");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }

    gif_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
    int error = 0;
    gif_detail::GifPtr gif{DGifOpen(&state, gif_detail::read_callback, &error)};
    if (!gif) {
      return std::unexpected{std::format("打开 GIF 失败，错误码: {}", error)};
    }
    if (DGifSlurp(gif.get()) != GIF_OK) {
      return std::unexpected{std::format("GIF 解码失败，错误码: {}", gif->Error)};
    }
    if (gif->ImageCount <= 0 || gif->SWidth <= 0 || gif->SHeight <= 0) {
      return std::unexpected{std::format("GIF 尺寸无效: {}", path_to_utf8(path))};
    }

    const auto width = static_cast<std::size_t>(gif->SWidth);
    const auto height = static_cast<std::size_t>(gif->SHeight);
    const auto stride = decoder_common::checked_rgba_stride(width, "GIF decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "GIF decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    std::vector<std::byte> rgba(*byte_count, std::byte{0xff});

    const auto& frame = gif->SavedImages[0];
    const auto* colors = gif_detail::active_color_map(gif.get(), frame);
    if (colors == nullptr || colors->ColorCount <= 0) {
      return std::unexpected{"GIF 缺少调色板。"};
    }
    const auto frame_width = frame.ImageDesc.Width;
    const auto frame_height = frame.ImageDesc.Height;
    if (frame_width <= 0 || frame_height <= 0 || frame.ImageDesc.Left < 0 || frame.ImageDesc.Top < 0 ||
        frame.ImageDesc.Left + frame_width > gif->SWidth || frame.ImageDesc.Top + frame_height > gif->SHeight) {
      return std::unexpected{"GIF 首帧范围无效。"};
    }

    for (int y = 0; y < frame_height; ++y) {
      const auto source_row = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame_width);
      const auto target_y = static_cast<std::size_t>(frame.ImageDesc.Top + y);
      for (int x = 0; x < frame_width; ++x) {
        const auto index = static_cast<unsigned char>(frame.RasterBits[source_row + static_cast<std::size_t>(x)]);
        if (index >= colors->ColorCount) {
          return std::unexpected{"GIF 像素索引超出调色板范围。"};
        }
        const auto& color = colors->Colors[index];
        const auto target_x = static_cast<std::size_t>(frame.ImageDesc.Left + x);
        const auto out = target_y * *stride + target_x * 4;
        rgba[out + 0] = static_cast<std::byte>(color.Red);
        rgba[out + 1] = static_cast<std::byte>(color.Green);
        rgba[out + 2] = static_cast<std::byte>(color.Blue);
        rgba[out + 3] = std::byte{0xff};
      }
    }

    auto image = decoder_common::make_rgba_image(width, height, std::move(rgba),
                                                 AlphaMode::none, "GIF decoder");
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "giflib"};
  }
};

}  // namespace awj
