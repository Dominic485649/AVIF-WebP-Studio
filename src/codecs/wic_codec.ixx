module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
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

struct ComApartment {
  ComApartment() noexcept
      : init{CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)},
        should_uninitialize{SUCCEEDED(init)} {}

  ~ComApartment() {
    if (should_uninitialize) {
      CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ComApartment(ComApartment&& other) noexcept
      : init{other.init}, should_uninitialize{other.should_uninitialize} {
    other.should_uninitialize = false;
  }

  ComApartment& operator=(ComApartment&& other) noexcept {
    if (this != &other) {
      if (should_uninitialize) {
        CoUninitialize();
      }
      init = other.init;
      should_uninitialize = other.should_uninitialize;
      other.should_uninitialize = false;
    }
    return *this;
  }

  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;
  }

  HRESULT init{S_FALSE};
  bool should_uninitialize{};
};

struct ProbeFrameResult {
  ComApartment com{};
  ComPtr<IWICImagingFactory> factory{};
  ComPtr<IWICBitmapDecoder> decoder{};
  ComPtr<IWICBitmapFrameDecode> frame{};
};

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
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return std::unexpected{std::format("WIC 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }
  return {};
}

std::expected<ProbeFrameResult, std::string> open_first_frame(const fs::path& path) {
  if (auto file_size = check_input_file_size(path); !file_size) {
    return std::unexpected{file_size.error()};
  }

  ProbeFrameResult result{};
  if (!result.com.usable()) {
    return std::unexpected{std::format("WIC COM 初始化失败: {}", hresult_message(result.com.init))};
  }

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
  UINT frame_index = 0;
  static constexpr std::wstring_view ico_extensions[] = {L".ico"};
  if (frame_count > 1 &&
      decoder_common::extension_is_one_of(path, ico_extensions)) {
    std::uint64_t best_area = 0;
    for (UINT i = 0; i < frame_count; ++i) {
      IWICBitmapFrameDecode* candidate = nullptr;
      hr = result.decoder->GetFrame(i, &candidate);
      ComPtr<IWICBitmapFrameDecode> candidate_frame{candidate};
      if (FAILED(hr) || candidate_frame == nullptr) {
        continue;
      }
      UINT width = 0;
      UINT height = 0;
      hr = candidate_frame->GetSize(&width, &height);
      if (FAILED(hr)) {
        continue;
      }
      const auto area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
      if (area > best_area) {
        best_area = area;
        frame_index = i;
      }
    }
  }

  IWICBitmapFrameDecode* raw_frame = nullptr;
  hr = result.decoder->GetFrame(frame_index, &raw_frame);
  if (FAILED(hr)) {
    return std::unexpected{std::format("WIC 读取首帧失败: {}", hresult_message(hr))};
  }
  result.frame.reset(raw_frame);
  return std::move(result);
}


bool is_rgba_half_format(const WICPixelFormatGUID& format) noexcept {
  return IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf) != FALSE;
}

float half_to_float(std::uint16_t bits) noexcept {
  const std::uint32_t sign = (bits >> 15) & 1u;
  const std::uint32_t exponent = (bits >> 10) & 0x1fu;
  const std::uint32_t mantissa = bits & 0x03ffu;
  const double s = sign ? -1.0 : 1.0;
  if (exponent == 0) {
    return static_cast<float>(s * std::ldexp(static_cast<double>(mantissa), -24));
  }
  if (exponent == 31) {
    return sign ? -std::numeric_limits<float>::infinity()
                : std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(s * std::ldexp(1.0 + static_cast<double>(mantissa) / 1024.0,
                                          static_cast<int>(exponent) - 15));
}

std::uint16_t pq_code_from_linear(double linear) noexcept {
  constexpr double source_reference_nits = 80.0;
  constexpr double pq_max_nits = 10000.0;
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 32.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 128.0;
  constexpr double c3 = 2392.0 / 128.0;
  const double nits = std::clamp(linear * source_reference_nits, 0.0, pq_max_nits);
  const double y = std::pow(nits / pq_max_nits, m1);
  const double pq = std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
  return static_cast<std::uint16_t>(std::clamp(std::lround(pq * 65535.0), 0l, 65535l));
}

std::expected<void, std::string> convert_scrgb_half_to_bt2020_pq(
    std::vector<std::byte>& pixels, UINT width, UINT height, std::size_t stride) {
  if (stride < static_cast<std::size_t>(width) * 4u * sizeof(std::uint16_t)) {
    return std::unexpected{"WIC scRGB half stride 无效。"};
  }
  for (UINT y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<std::uint16_t*>(pixels.data() + static_cast<std::size_t>(y) * stride);
    for (UINT x = 0; x < width; ++x) {
      auto* px = row + static_cast<std::size_t>(x) * 4u;
      const double sr = half_to_float(px[0]);
      const double sg = half_to_float(px[1]);
      const double sb = half_to_float(px[2]);
      const double alpha = std::clamp(static_cast<double>(half_to_float(px[3])), 0.0, 1.0);
      // scRGB is linear sRGB. Convert D65 linear sRGB -> XYZ -> BT.2020 before PQ coding.
      const double x_xyz = 0.41239079926595 * sr + 0.35758433938388 * sg + 0.18048078840183 * sb;
      const double y_xyz = 0.21263900587151 * sr + 0.71516867876776 * sg + 0.07219231536073 * sb;
      const double z_xyz = 0.01933081871559 * sr + 0.11919477979463 * sg + 0.95053215224966 * sb;
      const double r2020 = 1.71665118797127 * x_xyz - 0.35567078377639 * y_xyz - 0.25336628137366 * z_xyz;
      const double g2020 = -0.66668435183249 * x_xyz + 1.61648123663494 * y_xyz + 0.01576854581391 * z_xyz;
      const double b2020 = 0.01763985744531 * x_xyz - 0.04277061325781 * y_xyz + 0.94210312123547 * z_xyz;
      px[0] = pq_code_from_linear(r2020);
      px[1] = pq_code_from_linear(g2020);
      px[2] = pq_code_from_linear(b2020);
      px[3] = static_cast<std::uint16_t>(std::clamp(std::lround(alpha * 65535.0), 0l, 65535l));
    }
  }
  return {};
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
  if (context_count >
      static_cast<UINT>(encoding_defaults::wic_max_color_contexts)) {
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
    if (profile_size > encoding_defaults::codec_metadata_max_bytes) {
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

class WicImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "wic"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {
        L".bmp", L".dib", L".rle", L".jpg", L".jpeg", L".jpe", L".jfif",
        L".png", L".gif", L".ico", L".tif", L".tiff", L".wdp", L".hdp", L".jxr",
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

      const auto source_details =
          wic_detail::source_info_from_pixel_format(*frame_source->factory, source_pixel_format);
      auto output_source_info = source_details.source_info;
      const bool source_is_rgba_half = wic_detail::is_rgba_half_format(source_pixel_format);
      const bool wants_high_depth = source_is_rgba_half || output_source_info.bit_depth > 8;
      const auto target_format = source_is_rgba_half ? GUID_WICPixelFormat64bppRGBAHalf
                                 : (wants_high_depth ? GUID_WICPixelFormat64bppRGBA
                                                     : GUID_WICPixelFormat32bppRGBA);
      int output_bit_depth = wants_high_depth ? 16 : 8;

      wic_detail::ComPtr<IWICFormatConverter> converter;
      IWICFormatConverter* raw_converter = nullptr;
      hr = frame_source->factory->CreateFormatConverter(&raw_converter);
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 创建格式转换器失败: {}", wic_detail::hresult_message(hr))};
      }
      converter.reset(raw_converter);

      hr = converter->Initialize(frame_source->frame.get(), target_format,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
      if (FAILED(hr) && wants_high_depth) {
        output_bit_depth = 8;
        output_source_info.bit_depth = 8;
        output_source_info.has_hdr_metadata = false;
        output_source_info.color_metadata_source = "wic-sdr-fallback";
        hr = converter->Initialize(frame_source->frame.get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
      }
      if (FAILED(hr)) {
        return std::unexpected{std::format("WIC 转换 RGBA 失败: {}", wic_detail::hresult_message(hr))};
      }

      if (wants_high_depth && output_bit_depth == 16) {
        output_source_info.has_hdr_metadata = true;
        if (output_source_info.color_metadata_source.empty()) {
          output_source_info.color_metadata_source = "wic-high-bit-depth";
        }
      }

      const auto stride = decoder_common::checked_rgba_stride(
          width, "WIC decoder", output_bit_depth > 8 ? 2 : 1);
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

      const bool converted_sc_rgb_hdr = source_is_rgba_half && output_bit_depth == 16;
      if (converted_sc_rgb_hdr) {
        if (auto converted = wic_detail::convert_scrgb_half_to_bt2020_pq(*rgba, width, height, *stride);
            !converted) {
          return std::unexpected{converted.error()};
        }
        output_source_info.pixel_format = PixelFormat::rgba;
        output_source_info.bit_depth = 16;
        output_source_info.color_primaries = 9;
        output_source_info.transfer_characteristics = 16;
        output_source_info.matrix_coefficients = 9;
        output_source_info.color_range = 1;
        output_source_info.has_hdr_metadata = true;
        output_source_info.color_metadata_source = "wic-scrgb-half-bt2020-pq";
      }

      const auto alpha_mode = source_details.supports_transparency.value_or(true)
                                  ? AlphaMode::straight
                                  : AlphaMode::none;
      auto icc_profile = converted_sc_rgb_hdr
                             ? std::expected<std::vector<std::byte>, std::string>{std::vector<std::byte>{}}
                             : wic_detail::copy_icc_profile(*frame_source->factory,
                                                            *frame_source->frame);
      if (!icc_profile) {
        return std::unexpected{icc_profile.error()};
      }
      if (!icc_profile->empty()) {
        output_source_info.color_metadata_source = "source-icc";
      }
      auto image = decoder_common::make_rgba_image(width, height, std::move(*rgba), alpha_mode,
                                                   "WIC decoder", output_source_info,
                                                   output_bit_depth);
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
