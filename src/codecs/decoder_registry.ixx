module;

#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module awj.decoder_registry;

import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.gif_codec;
import awj.image;
import awj.large_image_plan;
import awj.jpeg_codec;
import awj.jxl_codec;
import awj.libraw_codec;
import awj.png_codec;
import awj.raw_codec;
import awj.tiff_codec;
import awj.webp_codec;
import awj.wic_codec;

export namespace awj {

struct DecoderRegistryOptions {
  bool allow_wic_fallback{true};
};

struct DecoderSelection {
  std::unique_ptr<ImageDecoder> decoder{};
  bool fallback{};
};

namespace decoder_registry_detail {

template <class Decoder>
bool try_select(const fs::path& path, DecoderSelection& selection) {
  auto decoder = std::make_unique<Decoder>();
  if (!decoder->can_decode(path)) {
    return false;
  }
  selection.decoder = std::move(decoder);
  return true;
}

}  // namespace decoder_registry_detail

export std::expected<DecoderSelection, std::string> select_decoder_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  DecoderSelection selection{};
  if (decoder_registry_detail::try_select<WebPImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<JXLImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<AvifImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<PngImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<JpegImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<GifImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<TiffImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<RawImageDecoder>(path, selection) ||
      decoder_registry_detail::try_select<LibRawImageDecoder>(path, selection)) {
    return selection;
  }

  if (options.allow_wic_fallback &&
      decoder_registry_detail::try_select<WicImageDecoder>(path, selection)) {
    selection.fallback = true;
    return selection;
  }

  return std::unexpected{std::format("native backend 暂不支持输入格式: {}", path.extension().string())};
}

export std::expected<ImageDimensions, std::string> probe_image_dimensions_for_path(
    const fs::path& path,
    DecoderRegistryOptions options) {
  auto selected = select_decoder_for_path(path, options);
  if (!selected) {
    return std::unexpected{selected.error()};
  }
  return selected->decoder->probe_dimensions(path);
}

}  // namespace awj
