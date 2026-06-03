module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>
#include <wincodec.h>

export module awj.wic_codec;

import awj.codec;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace wic_detail {

struct ComReleaser {
  template <class T>
  void operator()(T* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

template <class T>
using ComPtr = std::unique_ptr<T, ComReleaser>;

struct ProbeFrameResult {
  ComPtr<IWICImagingFactory> factory{};
  ComPtr<IWICBitmapDecoder> decoder{};
  ComPtr<IWICBitmapFrameDecode> frame{};
};

constexpr std::size_t max_metadata_bytes = 64 * 1024 * 1024;
constexpr UINT max_color_contexts = 16;

std::string hresult_message(HRESULT hr) {
  return std::format("HRESULT 0x{:08X}", static_cast<unsigned int>(hr));
}

std::expected<void, std::string> check_input_file_size(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return std::unexpected{std::format("读取 WIC 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::max_input_file_bytes)) {
    return std::unexpected{std::format("WIC 文件超过 20 GiB 输入上限: {}",
                                       display_path_for_user(path))};
  }
  return {};
}

std::expected<ProbeFrameResult, std::string> open_first_frame(const fs::path& path) {
  if (auto file_size = check_input_file_size(path); !file_size) {
    return std::unexpected{file_size.error()};
  }

  ProbeFrameResult result{};
  IWICImagingFactory* raw_factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&raw_factory));
  if (FAILED(hr)) {
    return std::unexpected{std::format("创建 WIC factory 失败: {}", hresult_message(hr))};
  }
  result.factory.reset(raw_factory);

  IWICBitmapDecoder* raw_decoder = nullptr;
  hr = result.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &raw_decoder);
  if (FAILED(hr)) {
    return std::unexpected{std::format("WIC 打开失败: {}", hresult_message(hr))};
  }
  result.decoder.reset(raw_decoder);

  UINT frame_count = 0;
  hr = result.decoder->GetFrameCount(&frame_count);
  if (FAILED(hr)) {
    return std::unexpected{std::format("WIC 读取帧数失败: {}", hresult_message(hr))};
  }
  if (frame_count == 0) {
    return std::unexpected{"WIC 不包含图像帧。"};
  }
  if (frame_count > 1) {
    return std::unexpected{std::format("暂不支持多帧 WIC 输入: {}", display_path_for_user(path))};
  }

  IWICBitmapFrameDecode* raw_frame = nullptr;
  hr = result.decoder->GetFrame(0, &raw_frame);
  if (FAILED(hr)) {
    return std::unexpected{std::format("WIC 读取首帧失败: {}", hresult_message(hr))};
  }
  result.frame.reset(raw_frame);
  return result;
}

struct PixelFormatDetails {
  ImageSourceInfo source_info{};
  std::optional<bool> supports_transparency{};
};

PixelFormat pixel_format_from_wic(UINT channel_count,
                                  bool supports_transparency,
                                  WICPixelFormatNumericRepresentation numeric_representation) noexcept {
  if (numeric_representation == WICPixelFormatNumericRepresentationIndexed) {
    return PixelFormat::rgb;
  }
  if (channel_count == 1) {
    return PixelFormat::gray;
  }
  if (channel_count == 3) {
    return PixelFormat::rgb;
  }
  if (channel_count == 4 && supports_transparency) {
    return PixelFormat::rgba;
  }
  return PixelFormat::unknown;
}

int bit_depth_from_wic(UINT bits_per_pixel,
                       UINT channel_count,
                       WICPixelFormatNumericRepresentation numeric_representation) noexcept {
  if (bits_per_pixel == 0 || bits_per_pixel > static_cast<UINT>(std::numeric_limits<int>::max())) {
    return 0;
  }
  if (numeric_representation == WICPixelFormatNumericRepresentationIndexed) {
    return static_cast<int>(bits_per_pixel);
  }
  if (channel_count == 0 || bits_per_pixel % channel_count != 0) {
    return 0;
  }
  const auto depth = bits_per_pixel / channel_count;
  return depth == 0 ? 0 : static_cast<int>(depth);
}

PixelFormatDetails source_info_from_pixel_format(IWICImagingFactory& factory,
                                                 const WICPixelFormatGUID& pixel_format) {
  PixelFormatDetails result{};
  IWICComponentInfo* raw_component_info = nullptr;
  HRESULT hr = factory.CreateComponentInfo(pixel_format, &raw_component_info);
  if (FAILED(hr)) {
    return result;
  }
  ComPtr<IWICComponentInfo> component_info{raw_component_info};

  IWICPixelFormatInfo2* raw_pixel_info = nullptr;
  hr = component_info->QueryInterface(IID_PPV_ARGS(&raw_pixel_info));
  if (FAILED(hr)) {
    return result;
  }
  ComPtr<IWICPixelFormatInfo2> pixel_info{raw_pixel_info};

  BOOL supports_transparency = FALSE;
  hr = pixel_info->SupportsTransparency(&supports_transparency);
  if (SUCCEEDED(hr)) {
    result.supports_transparency = supports_transparency != FALSE;
  }

  UINT bits_per_pixel = 0;
  if (FAILED(pixel_info->GetBitsPerPixel(&bits_per_pixel))) {
    bits_per_pixel = 0;
  }
  UINT channel_count = 0;
  if (FAILED(pixel_info->GetChannelCount(&channel_count))) {
    channel_count = 0;
  }
  WICPixelFormatNumericRepresentation numeric_representation =
      WICPixelFormatNumericRepresentationUnspecified;
  if (FAILED(pixel_info->GetNumericRepresentation(&numeric_representation))) {
    numeric_representation = WICPixelFormatNumericRepresentationUnspecified;
  }
  result.source_info = ImageSourceInfo{
      .pixel_format = pixel_format_from_wic(channel_count,
                                            result.supports_transparency.value_or(false),
                                            numeric_representation),
      .bit_depth = bit_depth_from_wic(bits_per_pixel, channel_count, numeric_representation)};
  return result;
}

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(
    IWICImagingFactory& factory,
    IWICBitmapFrameDecode& frame) {
  UINT context_count = 0;
  HRESULT hr = frame.GetColorContexts(0, nullptr, &context_count);
  if (FAILED(hr) || context_count == 0) {
    return std::vector<std::byte>{};
  }
  if (context_count > max_color_contexts) {
    return std::unexpected{"WIC 色彩上下文数量超过 16 个上限。"};
  }

  std::vector<ComPtr<IWICColorContext>> contexts{};
  std::vector<IWICColorContext*> raw_contexts{};
  contexts.reserve(context_count);
  raw_contexts.reserve(context_count);
  for (UINT index = 0; index < context_count; ++index) {
    IWICColorContext* raw_context = nullptr;
    hr = factory.CreateColorContext(&raw_context);
    if (FAILED(hr)) {
      return std::unexpected{std::format("WIC 创建色彩上下文失败: {}", hresult_message(hr))};
    }
    contexts.emplace_back(raw_context);
    raw_contexts.push_back(raw_context);
  }

  UINT actual_count = 0;
  hr = frame.GetColorContexts(context_count, raw_contexts.data(), &actual_count);
  if (FAILED(hr)) {
    return std::vector<std::byte>{};
  }
  for (UINT index = 0; index < actual_count && index < raw_contexts.size(); ++index) {
    WICColorContextType type = WICColorContextUninitialized;
    hr = raw_contexts[index]->GetType(&type);
    if (FAILED(hr) || type != WICColorContextProfile) {
      continue;
    }
    UINT profile_size = 0;
    hr = raw_contexts[index]->GetProfileBytes(0, nullptr, &profile_size);
    if (FAILED(hr) || profile_size == 0) {
      continue;
    }
    if (profile_size > max_metadata_bytes) {
      return std::unexpected{"WIC ICC profile 超过 64 MiB 上限。"};
    }
    auto bytes = decoder_common::make_byte_buffer(profile_size, "WIC ICC profile");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    hr = raw_contexts[index]->GetProfileBytes(profile_size,
                                              reinterpret_cast<BYTE*>(bytes->data()),
                                              &profile_size);
    if (FAILED(hr)) {
      return std::unexpected{std::format("WIC 读取 ICC profile 失败: {}", hresult_message(hr))};
    }
    if (profile_size > bytes->size()) {
      return std::unexpected{"WIC ICC profile 大小在读取期间发生变化。"};
    }
    if (profile_size < bytes->size()) {
      bytes->resize(profile_size);
    }
    return std::move(*bytes);
  }
  return std::vector<std::byte>{};
}

}  // namespace wic_detail

export class WicImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "wic"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {
        L".bmp", L".dib", L".rle", L".jpg", L".jpeg", L".jpe", L".jfif",
        L".png", L".gif", L".tif", L".tiff", L".wdp", L".hdp", L".jxr",
        L".heic", L".heif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto frame = wic_detail::open_first_frame(path);
      if (!frame) {
        return std::unexpected{frame.error()};
      }
      UINT width = 0;
      UINT height = 0;
      const HRESULT hr = frame->frame->GetSize(&width, &height);
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 尺寸无效: {}", wic_detail::hresult_message(hr))};
      }
      return decoder_common::make_image_dimensions_checked(width, height, "WIC");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WIC 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WIC 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"WIC 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto frame_source = wic_detail::open_first_frame(path);
      if (!frame_source) {
        return std::unexpected{frame_source.error()};
      }

      UINT width = 0;
      UINT height = 0;
      HRESULT hr = frame_source->frame->GetSize(&width, &height);
      if (FAILED(hr) || width == 0 || height == 0) {
        return std::unexpected{std::format("WIC 尺寸无效: {}", wic_detail::hresult_message(hr))};
      }

      WICPixelFormatGUID source_pixel_format{};
      hr = frame_source->frame->GetPixelFormat(&source_pixel_format);
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 像素格式无效: {}", wic_detail::hresult_message(hr))};
      }

      wic_detail::ComPtr<IWICFormatConverter> converter;
      IWICFormatConverter* raw_converter = nullptr;
      hr = frame_source->factory->CreateFormatConverter(&raw_converter);
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 创建格式转换器失败: {}", wic_detail::hresult_message(hr))};
      }
      converter.reset(raw_converter);

      hr = converter->Initialize(frame_source->frame.get(), GUID_WICPixelFormat32bppRGBA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 转换 RGBA 失败: {}", wic_detail::hresult_message(hr))};
      }

      const auto stride = decoder_common::checked_rgba_stride(width, "WIC decoder");
      if (!stride) {
        return std::unexpected{stride.error()};
      }
      const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "WIC decoder");
      if (!byte_count) {
        return std::unexpected{byte_count.error()};
      }
      if (*stride > std::numeric_limits<UINT>::max() || *byte_count > std::numeric_limits<UINT>::max()) {
        return std::unexpected{"WIC 输出 buffer 超过 API 限制。"};
      }

      auto rgba = decoder_common::make_byte_buffer(*byte_count, "WIC decoder");
      if (!rgba) {
        return std::unexpected{rgba.error()};
      }
      hr = converter->CopyPixels(nullptr, static_cast<UINT>(*stride), static_cast<UINT>(rgba->size()),
                                 reinterpret_cast<BYTE*>(rgba->data()));
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 复制像素失败: {}", wic_detail::hresult_message(hr))};
      }

      const auto source_details =
          wic_detail::source_info_from_pixel_format(*frame_source->factory, source_pixel_format);
      const auto alpha_mode = source_details.supports_transparency.value_or(true)
                                  ? AlphaMode::straight
                                  : AlphaMode::none;
      auto icc_profile = wic_detail::copy_icc_profile(*frame_source->factory,
                                                      *frame_source->frame);
      if (!icc_profile) {
        return std::unexpected{icc_profile.error()};
      }
      auto image = decoder_common::make_rgba_image(width, height, std::move(*rgba), alpha_mode,
                                                   "WIC decoder", source_details.source_info);
      if (!image) {
        return std::unexpected{image.error()};
      }
      if (!icc_profile->empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(*icc_profile)});
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "wic"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"WIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"WIC 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"WIC 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
