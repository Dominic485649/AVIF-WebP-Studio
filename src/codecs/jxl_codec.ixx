module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

export module awj.jxl_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace jxl_detail {

struct DecoderDeleter {
  void operator()(JxlDecoder* value) const noexcept {
    if (value != nullptr) {
      JxlDecoderDestroy(value);
    }
  }
};

struct EncoderDeleter {
  void operator()(JxlEncoder* value) const noexcept {
    if (value != nullptr) {
      JxlEncoderDestroy(value);
    }
  }
};

struct RunnerDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      JxlThreadParallelRunnerDestroy(value);
    }
  }
};

using DecoderPtr = std::unique_ptr<JxlDecoder, DecoderDeleter>;
using EncoderPtr = std::unique_ptr<JxlEncoder, EncoderDeleter>;
using RunnerPtr = std::unique_ptr<void, RunnerDeleter>;

std::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "JXL");
}

std::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context) {
  if (width > std::numeric_limits<std::size_t>::max() / 4) {
    return std::unexpected{std::format("{} 输入宽度过大。", context)};
  }
  return width * 4;
}

std::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  return stride * height;
}

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{"JXL encoder 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride = checked_rgba_stride(image.width, "JXL encoder");
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  if (plane.stride != *expected_stride) {
    return std::unexpected{"JXL encoder 当前需要紧凑排列的 RGBA buffer。"};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, "JXL encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"JXL encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

std::expected<RunnerPtr, std::string> create_runner(int requested_threads) {
  const auto threads = static_cast<std::size_t>(std::max(1, requested_threads));
  RunnerPtr runner{JxlThreadParallelRunnerCreate(nullptr, threads)};
  if (!runner) {
    return std::unexpected{"创建 JXL 线程 runner 失败。"};
  }
  return runner;
}

std::expected<void, std::string> attach_decoder_runner(JxlDecoder* decoder,
                                                       void* runner) {
  if (JxlDecoderSetParallelRunner(decoder, JxlThreadParallelRunner, runner) !=
      JXL_DEC_SUCCESS) {
    return std::unexpected{"设置 JXL decoder 线程 runner 失败。"};
  }
  return {};
}

std::expected<void, std::string> attach_encoder_runner(JxlEncoder* encoder,
                                                       void* runner) {
  if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) !=
      JXL_ENC_SUCCESS) {
    return std::unexpected{"设置 JXL encoder 线程 runner 失败。"};
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> collect_encoder_output(
    JxlEncoder* encoder) {
  std::vector<std::uint8_t> output(16 * 1024);
  auto* next_out = output.data();
  auto avail_out = output.size();

  while (true) {
    const auto status = JxlEncoderProcessOutput(encoder, &next_out, &avail_out);
    const auto used = output.size() - avail_out;
    if (status == JXL_ENC_SUCCESS) {
      output.resize(used);
      std::vector<std::byte> bytes(output.size());
      std::ranges::copy_n(reinterpret_cast<const std::byte*>(output.data()),
                          bytes.size(), bytes.begin());
      return bytes;
    }
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      return std::unexpected{std::format("JXL 编码失败，错误码: {}",
                                         static_cast<int>(JxlEncoderGetError(encoder)))};
    }

    const auto old_size = output.size();
    if (old_size > std::numeric_limits<std::size_t>::max() / 2) {
      return std::unexpected{"JXL 编码输出过大。"};
    }
    output.resize(old_size * 2);
    next_out = output.data() + used;
    avail_out = output.size() - used;
  }
}

}  // namespace jxl_detail

export class JXLImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libjxl"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".jxl";
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto bytes = jxl_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }

    jxl_detail::DecoderPtr decoder{JxlDecoderCreate(nullptr)};
    if (!decoder) {
      return std::unexpected{"创建 JXL decoder 失败。"};
    }
    auto runner = jxl_detail::create_runner(1);
    if (!runner) {
      return std::unexpected{runner.error()};
    }
    if (auto attached = jxl_detail::attach_decoder_runner(decoder.get(), runner->get());
        !attached) {
      return std::unexpected{attached.error()};
    }
    if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS) {
      return std::unexpected{"订阅 JXL decoder 事件失败。"};
    }
    const auto* input = reinterpret_cast<const std::uint8_t*>(bytes->data());
    if (JxlDecoderSetInput(decoder.get(), input, bytes->size()) != JXL_DEC_SUCCESS) {
      return std::unexpected{"设置 JXL 输入 buffer 失败。"};
    }
    JxlDecoderCloseInput(decoder.get());
    while (true) {
      const auto status = JxlDecoderProcessInput(decoder.get());
      if (status == JXL_DEC_ERROR) {
        return std::unexpected{std::format("JXL 读取尺寸失败: {}", path_to_utf8(path))};
      }
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        return std::unexpected{std::format("JXL 输入不完整: {}", path_to_utf8(path))};
      }
      if (status == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info{};
        if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
          return std::unexpected{std::format("JXL 基础信息无效: {}", path_to_utf8(path))};
        }
        return decoder_common::make_image_dimensions_checked(info.xsize, info.ysize,
                                                             "JXL");
      }
      if (status == JXL_DEC_SUCCESS) {
        return std::unexpected{std::format("JXL 未返回基础信息: {}", path_to_utf8(path))};
      }
    }
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes,
      std::string_view source_name) const override {
    return decode_bytes(bytes, source_name);
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    auto bytes = jxl_detail::read_file_bytes(path);
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    return decode_bytes(*bytes, path_to_utf8(path));
  }

 private:
  static std::expected<ImageDecodeResult, std::string> decode_bytes(
      std::span<const std::byte> bytes,
      std::string_view source_name) {
    jxl_detail::DecoderPtr decoder{JxlDecoderCreate(nullptr)};
    if (!decoder) {
      return std::unexpected{"创建 JXL decoder 失败。"};
    }

    auto runner = jxl_detail::create_runner(1);
    if (!runner) {
      return std::unexpected{runner.error()};
    }
    if (auto attached = jxl_detail::attach_decoder_runner(decoder.get(), runner->get());
        !attached) {
      return std::unexpected{attached.error()};
    }

    if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS) {
      return std::unexpected{"订阅 JXL decoder 事件失败。"};
    }

    const auto* input = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (JxlDecoderSetInput(decoder.get(), input, bytes.size()) != JXL_DEC_SUCCESS) {
      return std::unexpected{"设置 JXL 输入 buffer 失败。"};
    }
    JxlDecoderCloseInput(decoder.get());

    JxlBasicInfo info{};
    std::vector<std::byte> rgba;
    JxlPixelFormat format{.num_channels = 4,
                          .data_type = JXL_TYPE_UINT8,
                          .endianness = JXL_NATIVE_ENDIAN,
                          .align = 0};

    while (true) {
      const auto status = JxlDecoderProcessInput(decoder.get());
      if (status == JXL_DEC_ERROR) {
        return std::unexpected{std::format("JXL 解码失败: {}", source_name)};
      }
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        return std::unexpected{std::format("JXL 输入不完整: {}", source_name)};
      }
      if (status == JXL_DEC_BASIC_INFO) {
        if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS ||
            info.xsize == 0 || info.ysize == 0) {
          return std::unexpected{std::format("JXL 基础信息无效: {}", source_name)};
        }
        size_t output_size = 0;
        if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &output_size) !=
                JXL_DEC_SUCCESS ||
            output_size == 0) {
          return std::unexpected{"计算 JXL RGBA 输出 buffer 大小失败。"};
        }
        rgba.resize(output_size);
        if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, rgba.data(),
                                        rgba.size()) != JXL_DEC_SUCCESS) {
          return std::unexpected{"设置 JXL RGBA 输出 buffer 失败。"};
        }
      }
      if (status == JXL_DEC_FULL_IMAGE) {
        if (rgba.empty()) {
          return std::unexpected{"JXL 解码未产生 RGBA 输出。"};
        }
        const auto stride = jxl_detail::checked_rgba_stride(static_cast<std::size_t>(info.xsize), "JXL decoder");
        if (!stride) {
          return std::unexpected{stride.error()};
        }
        ImagePlane plane{.bytes = std::move(rgba), .stride = *stride};
        ImageBuffer image{.width = info.xsize,
                          .height = info.ysize,
                          .pixel_format = PixelFormat::rgba,
                          .alpha_mode = AlphaMode::straight,
                          .bit_depth = 8};
        image.planes.push_back(std::move(plane));
        return ImageDecodeResult{.image = std::move(image), .decoder_id = "libjxl"};
      }
      if (status == JXL_DEC_SUCCESS) {
        return std::unexpected{std::format("JXL 解码结束但未得到完整图像: {}",
                                           source_name)};
      }
    }
  }
};

export class JXLImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libjxl"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::jxl,
                             .features = CodecFeature::lossless |
                                         CodecFeature::alpha |
                                         CodecFeature::thread_control |
                                         CodecFeature::visual_quality_search,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const override {
    auto plane = jxl_detail::rgba_plane(image);
    if (!plane) {
      return std::unexpected{plane.error()};
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      return std::unexpected{"JXL encoder 输入尺寸超过 libjxl API 限制。"};
    }

    jxl_detail::EncoderPtr encoder{JxlEncoderCreate(nullptr)};
    if (!encoder) {
      return std::unexpected{"创建 JXL encoder 失败。"};
    }

    auto runner = jxl_detail::create_runner(settings.resources.encoder_threads_per_file);
    if (!runner) {
      return std::unexpected{runner.error()};
    }
    if (auto attached = jxl_detail::attach_encoder_runner(encoder.get(), runner->get());
        !attached) {
      return std::unexpected{attached.error()};
    }

    JxlBasicInfo info{};
    JxlEncoderInitBasicInfo(&info);
    info.xsize = static_cast<std::uint32_t>(image.width);
    info.ysize = static_cast<std::uint32_t>(image.height);
    info.bits_per_sample = 8;
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    info.alpha_premultiplied = JXL_FALSE;
    info.uses_original_profile = JXL_TRUE;

    if (JxlEncoderSetBasicInfo(encoder.get(), &info) != JXL_ENC_SUCCESS) {
      return std::unexpected{"设置 JXL 基础信息失败。"};
    }

    JxlColorEncoding color{};
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS) {
      return std::unexpected{"设置 JXL sRGB 色彩信息失败。"};
    }

    auto* frame_settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    if (frame_settings == nullptr) {
      return std::unexpected{"创建 JXL frame settings 失败。"};
    }

    const auto speed_mapping = map_jxl_speed_to_effort(settings.speed);
    const bool speed_explicit = settings.speed_explicit;
    if (speed_explicit &&
        JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                         speed_mapping.codec_value) != JXL_ENC_SUCCESS) {
      return std::unexpected{"设置 JXL effort 失败。"};
    }

    const bool lossless = settings.visual_quality == 100 || settings.quality >= 100;
    if (lossless) {
      if (JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL lossless 失败。"};
      }
    } else {
      const auto distance = JxlEncoderDistanceFromQuality(
          static_cast<float>(std::clamp(settings.quality, 1, 99)));
      if (JxlEncoderSetFrameDistance(frame_settings, distance) != JXL_ENC_SUCCESS) {
        return std::unexpected{"设置 JXL distance 失败。"};
      }
    }

    JxlPixelFormat format{.num_channels = 4,
                          .data_type = JXL_TYPE_UINT8,
                          .endianness = JXL_NATIVE_ENDIAN,
                          .align = 0};
    const auto& rgba = (*plane)->bytes;
    const auto size = jxl_detail::checked_image_bytes((*plane)->stride, image.height, "JXL encoder");
    if (!size) {
      return std::unexpected{size.error()};
    }
    if (JxlEncoderAddImageFrame(frame_settings, &format, rgba.data(), *size) !=
        JXL_ENC_SUCCESS) {
      return std::unexpected{"添加 JXL RGBA frame 失败。"};
    }
    JxlEncoderCloseInput(encoder.get());

    auto output = jxl_detail::collect_encoder_output(encoder.get());
    if (!output) {
      return std::unexpected{output.error()};
    }

    return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*output),
                                                      .codec_name = "libjxl"},
                              .diagnostics = EncodeDiagnostics{
                                  .encoder_id = "libjxl",
                                  .fallback_reason = settings.jxl_jpeg_lossless_candidate
                                                         ? "JPEG lossless transcode requested, but this build path uses RGBA fallback because direct JPEG frame API was not safely available."
                                                         : std::string{},
                                  .speed_mapping = speed_mapping,
                                  .encoder_threads = settings.resources.encoder_threads_per_file,
                                  .memory_budget_bytes = settings.resources.memory_limit_bytes},
                              .final_quality = lossless ? 100 : settings.quality,
                              .lossless = lossless,
                              .search_attempt_count = 1};
  }
};

}  // namespace awj
