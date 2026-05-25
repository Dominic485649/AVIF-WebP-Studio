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
#include <windows.h>
#include <wincodec.h>

export module awj.wic_codec;

import awj.codec;
import awj.decoder_common;
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

std::string hresult_message(HRESULT hr) {
  return std::format("HRESULT 0x{:08X}", static_cast<unsigned int>(hr));
}

std::expected<ProbeFrameResult, std::string> open_first_frame(const fs::path& path) {
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

  IWICBitmapFrameDecode* raw_frame = nullptr;
  hr = result.decoder->GetFrame(0, &raw_frame);
  if (FAILED(hr)) {
    return std::unexpected{std::format("WIC 读取首帧失败: {}", hresult_message(hr))};
  }
  result.frame.reset(raw_frame);
  return result;
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
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
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

    std::vector<std::byte> rgba(*byte_count);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(*stride), static_cast<UINT>(rgba.size()),
                               reinterpret_cast<BYTE*>(rgba.data()));
    if (FAILED(hr)) {
      return std::unexpected{std::format("WIC 复制像素失败: {}", wic_detail::hresult_message(hr))};
    }

    auto image = decoder_common::make_rgba_image(width, height, std::move(rgba),
                                                 AlphaMode::straight, "WIC decoder");
    if (!image) {
      return std::unexpected{image.error()};
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "wic", .used_fallback = true};
  }
};

}  // namespace awj
