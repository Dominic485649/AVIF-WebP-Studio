module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <png.h>

export module awj.png_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace png_detail {

constexpr std::size_t max_metadata_bytes = 64 * 1024 * 1024;
constexpr png_uint_32 max_cached_metadata_chunks = 64;

std::uint32_t read_be_u32(std::span<const std::byte> bytes,
                          std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

bool chunk_type_is(std::span<const std::byte> header, std::string_view type) noexcept {
  return header.size() >= 8 &&
         header[4] == std::byte{static_cast<unsigned char>(type[0])} &&
         header[5] == std::byte{static_cast<unsigned char>(type[1])} &&
         header[6] == std::byte{static_cast<unsigned char>(type[2])} &&
         header[7] == std::byte{static_cast<unsigned char>(type[3])};
}

std::expected<bool, std::string> contains_animation_control_chunk(
    std::span<const std::byte> bytes,
    std::string_view source_name) {
  if (bytes.size() < 8 ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) != 0) {
    return std::unexpected{std::format("PNG 签名无效: {}", source_name)};
  }

  std::size_t offset = 8;
  while (offset + 12 <= bytes.size()) {
    const auto chunk_size = read_be_u32(bytes, offset);
    if (chunk_size > bytes.size() - offset - 12) {
      return std::unexpected{std::format("PNG 文件截断: {}", source_name)};
    }
    const auto header = bytes.subspan(offset, 8);
    if (chunk_type_is(header, "acTL")) {
      return true;
    }
    if (chunk_type_is(header, "IDAT")) {
      return false;
    }
    offset += 12 + chunk_size;
  }
  return std::unexpected{std::format("PNG 文件截断: {}", source_name)};
}

std::expected<bool, std::string> file_contains_animation_control_chunk(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return std::unexpected{std::format("读取 PNG 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::max_input_file_bytes)) {
    return std::unexpected{std::format("PNG 文件超过 20 GiB 输入上限: {}",
                                       display_path_for_user(path))};
  }

  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 PNG 文件: {}", display_path_for_user(path))};
  }

  std::array<std::byte, 8> signature{};
  input.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
  if (input.gcount() != static_cast<std::streamsize>(signature.size()) || input.bad() ||
      png_sig_cmp(reinterpret_cast<png_const_bytep>(signature.data()), 0, 8) != 0) {
    return std::unexpected{std::format("PNG 签名无效: {}", display_path_for_user(path))};
  }

  while (true) {
    std::array<std::byte, 8> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) || input.bad()) {
      return std::unexpected{std::format("PNG 文件截断: {}", display_path_for_user(path))};
    }
    if (chunk_type_is(header, "acTL")) {
      return true;
    }
    if (chunk_type_is(header, "IDAT")) {
      return false;
    }
    const auto chunk_size = read_be_u32(header, 0);
    input.seekg(static_cast<std::streamoff>(chunk_size) + 4, std::ios::cur);
    if (!input) {
      return std::unexpected{std::format("PNG 文件截断: {}", display_path_for_user(path))};
    }
  }
}

struct ReadState {
  const std::byte* data{};
  std::size_t size{};
  std::size_t offset{};
};

struct DecodeContext {
  std::vector<std::byte> rgba{};
  std::vector<png_bytep> rows{};
  std::vector<std::byte> icc_profile{};
  std::vector<std::byte> exif_metadata{};
  std::vector<std::byte> xmp_metadata{};
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

std::expected<std::vector<std::byte>, std::string> copy_metadata_payload(
    const void* data,
    std::size_t size,
    std::string_view context) {
  if (data == nullptr || size == 0) {
    return std::vector<std::byte>{};
  }
  if (size > max_metadata_bytes) {
    return std::unexpected{std::format("{} 超过 64 MiB 上限。", context)};
  }
  auto bytes = decoder_common::make_byte_buffer(size, context);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::ranges::copy_n(static_cast<const std::byte*>(data), size, bytes->begin());
  return std::move(*bytes);
}

void clamp_chunk_limits(png_structp png) noexcept {
#ifdef PNG_SET_USER_LIMITS_SUPPORTED
  if (png == nullptr) {
    return;
  }
  const auto current_cache_limit = png_get_chunk_cache_max(png);
  if (current_cache_limit == 0 || current_cache_limit > max_cached_metadata_chunks) {
    png_set_chunk_cache_max(png, max_cached_metadata_chunks);
  }
  constexpr auto max_chunk_malloc_bytes = static_cast<png_alloc_size_t>(max_metadata_bytes);
  const auto current_malloc_limit = png_get_chunk_malloc_max(png);
  if (current_malloc_limit == 0 || current_malloc_limit > max_chunk_malloc_bytes) {
    png_set_chunk_malloc_max(png, max_chunk_malloc_bytes);
  }
#else
  (void)png;
#endif
}

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(png_structp png,
                                                                     png_infop info) {
  png_charp name = nullptr;
  int compression_type = 0;
  png_bytep profile = nullptr;
  png_uint_32 profile_size = 0;
  if (png_get_iCCP(png, info, &name, &compression_type, &profile, &profile_size) == 0 ||
      profile == nullptr || profile_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(profile, profile_size, "PNG ICC profile");
}

std::expected<std::vector<std::byte>, std::string> copy_exif_metadata(png_structp png,
                                                                      png_infop info) {
  png_uint_32 exif_size = 0;
  png_bytep exif = nullptr;
  if (png_get_eXIf_1(png, info, &exif_size, &exif) == 0 || exif == nullptr || exif_size == 0) {
    return std::vector<std::byte>{};
  }
  return copy_metadata_payload(exif, exif_size, "PNG Exif metadata");
}

std::expected<std::vector<std::byte>, std::string> copy_exif_metadata(png_structp png,
                                                                      png_infop info,
                                                                      png_infop end_info) {
  auto bytes = copy_exif_metadata(png, info);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (!bytes->empty() || end_info == nullptr) {
    return std::move(*bytes);
  }
  return copy_exif_metadata(png, end_info);
}

std::size_t text_payload_size(const png_text& text) noexcept {
  switch (text.compression) {
    case PNG_ITXT_COMPRESSION_NONE:
    case PNG_ITXT_COMPRESSION_zTXt:
      return text.itxt_length;
    case PNG_TEXT_COMPRESSION_NONE:
    case PNG_TEXT_COMPRESSION_zTXt:
      return text.text_length;
    default:
      return 0;
  }
}

std::expected<std::vector<std::byte>, std::string> copy_xmp_metadata(png_structp png,
                                                                     png_infop info) {
  static constexpr std::string_view xmp_keyword = "XML:com.adobe.xmp";

  png_textp text = nullptr;
  int text_count = 0;
  if (png_get_text(png, info, &text, &text_count) <= 0 || text == nullptr) {
    return std::vector<std::byte>{};
  }
  for (int index = 0; index < text_count; ++index) {
    const auto& entry = text[index];
    if (entry.key == nullptr || std::string_view{entry.key} != xmp_keyword || entry.text == nullptr) {
      continue;
    }
    const auto payload_size = text_payload_size(entry);
    if (payload_size == 0) {
      continue;
    }
    return copy_metadata_payload(entry.text, payload_size, "PNG XMP metadata");
  }
  return std::vector<std::byte>{};
}

std::expected<std::vector<std::byte>, std::string> copy_xmp_metadata(png_structp png,
                                                                     png_infop info,
                                                                     png_infop end_info) {
  auto bytes = copy_xmp_metadata(png, info);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  if (!bytes->empty() || end_info == nullptr) {
    return std::move(*bytes);
  }
  return copy_xmp_metadata(png, end_info);
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
    try {
      auto bytes = decoder_common::read_file_prefix(path, 24, "PNG");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      if (bytes->size() < 24 ||
          png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0 ||
          png_detail::read_be_u32(*bytes, 8) != 13 ||
          (*bytes)[12] != std::byte{'I'} || (*bytes)[13] != std::byte{'H'} ||
          (*bytes)[14] != std::byte{'D'} || (*bytes)[15] != std::byte{'R'}) {
        return std::unexpected{std::format("PNG 文件头无效: {}", display_path_for_user(path))};
      }
      auto is_animated = png_detail::file_contains_animation_control_chunk(path);
      if (!is_animated) {
        return std::unexpected{is_animated.error()};
      }
      if (*is_animated) {
        return std::unexpected{std::format("暂不支持动画 PNG 输入: {}", display_path_for_user(path))};
      }
      return decoder_common::make_image_dimensions_checked(png_detail::read_be_u32(*bytes, 16),
                                                           png_detail::read_be_u32(*bytes, 20),
                                                           "PNG");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"PNG 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "PNG");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      if (bytes->size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes->data()), 0, 8) != 0) {
        return std::unexpected{std::format("PNG 签名无效: {}", display_path_for_user(path))};
      }
      auto is_animated = png_detail::contains_animation_control_chunk(*bytes, display_path_for_user(path));
      if (!is_animated) {
        return std::unexpected{is_animated.error()};
      }
      if (*is_animated) {
        return std::unexpected{std::format("暂不支持动画 PNG 输入: {}", display_path_for_user(path))};
      }

      std::unique_ptr<png_detail::DecodeContext> context;
      try {
        context = std::make_unique<png_detail::DecodeContext>();
      } catch (const std::bad_alloc&) {
        return std::unexpected{"PNG decoder 内存不足。"};
      }

      png_detail::PngReadPtr png{png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr)};
      if (!png) {
        return std::unexpected{"创建 PNG decoder 失败。"};
      }
      png_detail::clamp_chunk_limits(png.get());
      png_detail::PngInfoPtr info{png_create_info_struct(png.get()), png_detail::PngInfoDeleter{.png = png.get()}};
      if (!info) {
        return std::unexpected{"创建 PNG info 失败。"};
      }
      png_detail::PngInfoPtr end_info{png_create_info_struct(png.get()),
                                       png_detail::PngInfoDeleter{.png = png.get()}};
      if (!end_info) {
        return std::unexpected{"创建 PNG end info 失败。"};
      }

      png_detail::ReadState state{.data = bytes->data(), .size = bytes->size()};
      png_set_read_fn(png.get(), &state, png_detail::read_callback);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
      if (setjmp(png_jmpbuf(png.get())) != 0) {
        return std::unexpected{std::format("PNG 解码失败: {}", display_path_for_user(path))};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

      png_read_info(png.get(), info.get());
      png_uint_32 width = 0;
      png_uint_32 height = 0;
      int bit_depth = 0;
      int color_type = 0;
      png_get_IHDR(png.get(), info.get(), &width, &height, &bit_depth, &color_type,
                   nullptr, nullptr, nullptr);
      if (width == 0 || height == 0) {
        return std::unexpected{std::format("PNG 尺寸无效: {}", display_path_for_user(path))};
      }

      const bool source_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                                    png_get_valid(png.get(), info.get(), PNG_INFO_tRNS) != 0;

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

      std::size_t stride = 0;
      {
        const auto checked_stride = decoder_common::checked_rgba_stride(width, "PNG decoder");
        if (!checked_stride) {
          return std::unexpected{checked_stride.error()};
        }
        stride = *checked_stride;
      }
      if (png_get_rowbytes(png.get(), info.get()) != stride) {
        return std::unexpected{std::format("PNG 转换后行字节数无效: {}", display_path_for_user(path))};
      }
      {
        const auto byte_count = decoder_common::checked_image_bytes(stride, height, "PNG decoder");
        if (!byte_count) {
          return std::unexpected{byte_count.error()};
        }
        auto rgba = decoder_common::make_byte_buffer(*byte_count, "PNG decoder");
        if (!rgba) {
          return std::unexpected{rgba.error()};
        }
        context->rgba = std::move(*rgba);
      }
      const auto row_pointer_bytes = static_cast<std::uint64_t>(height) * sizeof(png_bytep);
      if (row_pointer_bytes > encoding_defaults::max_input_file_bytes) {
        return std::unexpected{"PNG decoder 行指针 buffer 超过 20 GiB 运行时上限。"};
      }
      try {
        context->rows.resize(height);
      } catch (const std::bad_alloc&) {
        return std::unexpected{"PNG decoder 行指针内存不足。"};
      } catch (const std::length_error&) {
        return std::unexpected{"PNG decoder 行指针数量超过运行时限制。"};
      }
      for (png_uint_32 y = 0; y < height; ++y) {
        context->rows[y] = reinterpret_cast<png_bytep>(context->rgba.data() + y * stride);
      }
      png_read_image(png.get(), context->rows.data());
      png_read_end(png.get(), end_info.get());

      {
        auto icc_profile = png_detail::copy_icc_profile(png.get(), info.get());
        if (!icc_profile) {
          return std::unexpected{icc_profile.error()};
        }
        context->icc_profile = std::move(*icc_profile);
      }
      {
        auto exif_metadata = png_detail::copy_exif_metadata(png.get(), info.get(), end_info.get());
        if (!exif_metadata) {
          return std::unexpected{exif_metadata.error()};
        }
        context->exif_metadata = std::move(*exif_metadata);
      }
      {
        auto xmp_metadata = png_detail::copy_xmp_metadata(png.get(), info.get(), end_info.get());
        if (!xmp_metadata) {
          return std::unexpected{xmp_metadata.error()};
        }
        context->xmp_metadata = std::move(*xmp_metadata);
      }

      auto image = decoder_common::make_rgba_image(
          width, height, std::move(context->rgba),
          source_has_alpha ? AlphaMode::straight : AlphaMode::none, "PNG decoder",
          ImageSourceInfo{.pixel_format = png_detail::source_pixel_format_for_png(color_type),
                          .bit_depth = bit_depth});
      if (!image) {
        return std::unexpected{image.error()};
      }
      if (!context->icc_profile.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(context->icc_profile)});
      }
      if (!context->exif_metadata.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::exif,
                                                .bytes = std::move(context->exif_metadata)});
      }
      if (!context->xmp_metadata.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::xmp,
                                                .bytes = std::move(context->xmp_metadata)});
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libpng"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"PNG 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"PNG 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"PNG 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
