#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.avif_aom_codec;
import awj.image;
import awj.jpegli_codec;
import awj.jxl_codec;
import awj.native_backend;
import awj.resource_planner;
import awj.webp_codec;

namespace {

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
}

awj::ImageBuffer make_test_image() {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                          .height = 2,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

awj::ImageBuffer make_alpha_test_image() {
  constexpr std::uint32_t width = 32;
  constexpr std::uint32_t height = 32;
  awj::ImagePlane plane{.stride = width * 4};
  plane.bytes.reserve(static_cast<std::size_t>(plane.stride) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 17u + y * 31u + 5u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 97u + y * 11u + x * y * 3u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          (x * 7u + y * 89u + x * y * 13u) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>(
          1u + ((x * 73u + y * 151u + x * y * 19u) % 254u))});
    }
  }
  awj::ImageBuffer image{.width = width,
                         .height = height,
                         .pixel_format = awj::PixelFormat::rgba,
                         .alpha_mode = awj::AlphaMode::straight,
                         .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

bool same_rgba(const awj::ImageBuffer& lhs, const awj::ImageBuffer& rhs) {
  if (lhs.width != rhs.width || lhs.height != rhs.height ||
      lhs.pixel_format != awj::PixelFormat::rgba ||
      rhs.pixel_format != awj::PixelFormat::rgba || lhs.planes.empty() ||
      rhs.planes.empty()) {
    return false;
  }
  const auto& lhs_plane = lhs.planes.front();
  const auto& rhs_plane = rhs.planes.front();
  const auto row_bytes = lhs.width * 4;
  for (std::size_t y = 0; y < lhs.height; ++y) {
    const auto lhs_row = std::span<const std::byte>{lhs_plane.bytes}.subspan(
        y * lhs_plane.stride, row_bytes);
    const auto rhs_row = std::span<const std::byte>{rhs_plane.bytes}.subspan(
        y * rhs_plane.stride, row_bytes);
    if (!std::ranges::equal(lhs_row, rhs_row)) {
      return false;
    }
  }
  return true;
}

void push_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8)});
}

awj::ImageBuffer make_hdr_test_image() {
  awj::ImagePlane plane{.stride = 16};
  for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{32768},
                                    std::uint16_t{65535}, std::uint16_t{65535},
                                    std::uint16_t{65535}, std::uint16_t{0},
                                    std::uint16_t{32768}, std::uint16_t{65535}}) {
    push_u16(plane.bytes, value);
  }
  awj::ImageBuffer image{.width = 2,
                          .height = 1,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 16};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "awjimage-native-pipeline-test";
  const auto input = root / "input.webp";
  const auto output = root / "out";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return fail("failed to create temp dir.");
  }

  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      make_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 100,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  {
    std::ofstream stream{input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::AppConfig cfg;
  cfg.input_path = input;
  cfg.output_dir = output;
  cfg.output_format = awj::OutputFormat::webp;
  cfg.quality = 100;
  cfg.max_jobs = 1;

  awj::FileLogger logger{output, false};
  awj::NativeBackend backend{cfg, logger, awj::ResourcePlan{
                                               .file_parallelism = 1,
                                               .encoder_threads_per_file = 1,
                                               .global_thread_budget = 1}};
  const auto input_bytes = std::filesystem::file_size(input, ec);
  if (ec) {
    return fail("failed to stat input file.");
  }
  auto result = backend.encode(awj::ImageFile{.index = 0,
                                               .path = input,
                                               .bytes = input_bytes});
  if (!result.ok || result.skipped || result.canceled) {
    return fail(result.message.empty() ? "native backend encode failed." : result.message);
  }
  const auto output_file = output / "input.webp";
  if (!std::filesystem::exists(output_file)) {
    return fail("native backend did not create output file.");
  }
  if (result.output_path != output_file || result.output_bytes == 0) {
    return fail("native backend result metadata invalid.");
  }

  const auto hdr_input = root / "input-hdr.jxl";
  awj::JXLImageEncoder jxl_encoder;
  auto hdr_encoded = jxl_encoder.encode(
      make_hdr_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!hdr_encoded) {
    return fail(hdr_encoded.error());
  }
  {
    std::ofstream stream{hdr_input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(hdr_encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(hdr_encoded->encoded.bytes.size()));
  }

  auto hdr_cfg = cfg;
  hdr_cfg.input_path = hdr_input;
  hdr_cfg.output_format = awj::OutputFormat::webp;
  hdr_cfg.quality = 100;
  hdr_cfg.bit_depth = {};
  hdr_cfg.visual_quality = {};
  awj::NativeBackend hdr_backend{hdr_cfg, logger, awj::ResourcePlan{
                                                   .file_parallelism = 1,
                                                   .encoder_threads_per_file = 1,
                                                   .global_thread_budget = 1}};
  const auto hdr_input_bytes = std::filesystem::file_size(hdr_input, ec);
  if (ec) {
    return fail("failed to stat HDR input file.");
  }
  auto hdr_result = hdr_backend.encode(awj::ImageFile{.index = 0,
                                                      .path = hdr_input,
                                                      .bytes = hdr_input_bytes});
  if (!hdr_result.ok || hdr_result.skipped || hdr_result.canceled) {
    return fail(hdr_result.message.empty() ? "native backend HDR fallback encode failed."
                                           : hdr_result.message);
  }
  if (hdr_result.source_bit_depth.value_or(0) != 16 ||
      hdr_result.applied_bit_depth.value_or(0) != 8 ||
      !hdr_result.source_has_hdr_metadata ||
      hdr_result.applied_hdr_metadata != "sdr-fallback" ||
      hdr_result.fallback_reason != "HDR -> SDR fallback") {
    return fail("native backend HDR fallback diagnostics invalid.");
  }

  cfg.output_format = awj::OutputFormat::jxl;
  awj::NativeBackend jxl_backend{cfg, logger, awj::ResourcePlan{
                                                  .file_parallelism = 1,
                                                  .encoder_threads_per_file = 1,
                                                  .global_thread_budget = 1}};
  auto jxl_result = jxl_backend.encode(awj::ImageFile{.index = 0,
                                                      .path = input,
                                                      .bytes = input_bytes});
  if (!jxl_result.ok || jxl_result.skipped || jxl_result.canceled) {
    return fail(jxl_result.message.empty() ? "native backend JXL encode failed."
                                           : jxl_result.message);
  }
  const auto jxl_output_file = output / "input.jxl";
  if (!std::filesystem::exists(jxl_output_file)) {
    return fail("native backend did not create JXL output file.");
  }
  if (jxl_result.output_path != jxl_output_file || jxl_result.output_bytes == 0 ||
      jxl_result.command.find("jxl") == std::string::npos) {
    return fail("native backend JXL result metadata invalid.");
  }

#if AWJ_HAS_JPEGLI
  const auto jpeg_input = root / "photo.jpg";
  awj::JpegliImageEncoder source_jpeg_encoder;
  auto source_jpeg = source_jpeg_encoder.encode(
      make_test_image(),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jpgli,
                                 .quality = awj::encoding_defaults::default_jpegli_quality,
                                 .speed = awj::default_speed_for(awj::OutputFormat::jpgli),
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!source_jpeg) {
    return fail(source_jpeg.error());
  }
  {
    std::ofstream stream{jpeg_input, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(source_jpeg->encoded.bytes.data()),
                 static_cast<std::streamsize>(source_jpeg->encoded.bytes.size()));
  }
  const auto jpeg_bytes = static_cast<std::uint64_t>(
      std::filesystem::file_size(jpeg_input, ec));
  cfg.output_format = awj::OutputFormat::jxl;
  cfg.quality = awj::encoding_defaults::default_jxl_quality;
  cfg.visual_quality = {};
  cfg.strip_metadata = false;
  awj::NativeBackend jxl_jpeg_backend{cfg, logger, awj::ResourcePlan{
                                                       .file_parallelism = 1,
                                                       .encoder_threads_per_file = 1,
                                                       .global_thread_budget = 1}};
  auto jxl_jpeg_result = jxl_jpeg_backend.encode(
      awj::ImageFile{.index = 0, .path = jpeg_input, .bytes = jpeg_bytes});
  if (!jxl_jpeg_result.ok ||
      jxl_jpeg_result.integration_mode != "jxl-jpeg-bitstream-transcode" ||
      !jxl_jpeg_result.lossless ||
      jxl_jpeg_result.final_encoder_quality != 100) {
    return fail("native backend JPEG->JXL did not use bitstream transcode.");
  }

  cfg.strip_metadata = true;
  awj::NativeBackend jxl_strip_backend{cfg, logger, awj::ResourcePlan{
                                                        .file_parallelism = 1,
                                                        .encoder_threads_per_file = 1,
                                                        .global_thread_budget = 1}};
  auto jxl_strip_result = jxl_strip_backend.encode(
      awj::ImageFile{.index = 0, .path = jpeg_input, .bytes = jpeg_bytes});
  if (!jxl_strip_result.ok ||
      jxl_strip_result.integration_mode == "jxl-jpeg-bitstream-transcode" ||
      jxl_strip_result.lossless ||
      jxl_strip_result.final_encoder_quality != awj::encoding_defaults::default_jxl_quality) {
    return fail("native backend JPEG->JXL strip fallback did not use default JXL lossy encode.");
  }
  cfg.strip_metadata = false;

  cfg.output_format = awj::OutputFormat::jpgli;
  cfg.quality = awj::encoding_defaults::default_jpegli_quality;
  cfg.bit_depth = 8;
  cfg.visual_quality = {};
  awj::NativeBackend jpegli_backend{cfg, logger, awj::ResourcePlan{
                                                     .file_parallelism = 1,
                                                     .encoder_threads_per_file = 1,
                                                     .global_thread_budget = 1}};
  auto jpegli_result = jpegli_backend.encode(awj::ImageFile{.index = 0,
                                                            .path = input,
                                                            .bytes = input_bytes});
  if (!jpegli_result.ok || jpegli_result.skipped || jpegli_result.canceled) {
    return fail(jpegli_result.message.empty() ? "native backend JPGLI encode failed."
                                              : jpegli_result.message);
  }
  const auto jpegli_output_file = output / "input.jpg";
  if (!std::filesystem::exists(jpegli_output_file)) {
    return fail("native backend did not create JPGLI .jpg output file.");
  }
  if (jpegli_result.output_path != jpegli_output_file ||
      jpegli_result.output_bytes == 0 ||
      jpegli_result.output_format != "JPGLI" ||
      jpegli_result.encoder_id != "jpegli" ||
      jpegli_result.command.find("JPGLI") == std::string::npos) {
    return fail("native backend JPGLI result metadata invalid.");
  }
  const auto jpegli_summary_dir = output / "jpegli-summary";
  if (auto csv_ok = awj::write_csv(
          jpegli_summary_dir,
          std::span<const awj::EncodeResult>{&jpegli_result, 1});
      !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto jpegli_summary_csv = read_text(jpegli_summary_dir / "summary.csv");
  if (jpegli_summary_csv.find("format,encoder_id") == std::string::npos ||
      jpegli_summary_csv.find("JPGLI,jpegli") == std::string::npos) {
    return fail("native backend JPGLI summary diagnostics invalid.");
  }
#endif

  cfg.output_format = awj::OutputFormat::avif;
  cfg.avif_encoder = awj::AvifEncoderMode::automatic;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.bit_depth = 8;
  cfg.visual_quality = {};
  awj::NativeBackend avif_backend{cfg, logger, awj::ResourcePlan{
                                                    .file_parallelism = 1,
                                                    .encoder_threads_per_file = 1,
                                                    .global_thread_budget = 1}};
  auto avif_result = avif_backend.encode(awj::ImageFile{.index = 0,
                                                         .path = input,
                                                         .bytes = input_bytes});
  if (!avif_result.ok || avif_result.skipped || avif_result.canceled) {
    return fail(avif_result.message.empty() ? "native backend AVIF encode failed."
                                            : avif_result.message);
  }
  const auto avif_output_file = output / "input.avif";
  if (!std::filesystem::exists(avif_output_file)) {
    return fail("native backend did not create AVIF output file.");
  }
  if (avif_result.output_path != avif_output_file || avif_result.output_bytes == 0 ||
      avif_result.command.find("aom") == std::string::npos ||
      avif_result.requested_encoder_id != "auto" || avif_result.encoder_id != "aom") {
    return fail("native backend AVIF auto result metadata invalid.");
  }

  const auto alpha_source = make_alpha_test_image();
  const auto alpha_input = root / "alpha-input.webp";
  auto alpha_webp = encoder.encode(
      alpha_source,
      awj::NativeEncodeSettings{
          .output_format = awj::OutputFormat::webp,
          .quality = 100,
          .speed = 10,
          .source_has_alpha_channel = true,
          .encoder_supports_alpha = true,
          .applied_alpha = "kept",
          .resources = awj::ResourcePlan{.file_parallelism = 1,
                                         .encoder_threads_per_file = 1,
                                         .global_thread_budget = 1}});
  if (!alpha_webp) {
    return fail(alpha_webp.error());
  }
  {
    std::ofstream stream{alpha_input, std::ios::binary};
    stream.write(
        reinterpret_cast<const char*>(alpha_webp->encoded.bytes.data()),
        static_cast<std::streamsize>(alpha_webp->encoded.bytes.size()));
  }
  awj::WebPImageDecoder webp_decoder;
  auto alpha_reference = webp_decoder.decode(alpha_input);
  if (!alpha_reference ||
      alpha_reference->image.alpha_mode == awj::AlphaMode::none) {
    return fail(alpha_reference ? "WebP alpha fixture lost its alpha channel."
                                : alpha_reference.error());
  }

  auto alpha_cfg = cfg;
  alpha_cfg.input_path = alpha_input;
  alpha_cfg.output_dir = output / "alpha-auto";
  alpha_cfg.quality = 70;
  alpha_cfg.visual_quality = {};
  alpha_cfg.speed = 5;
  alpha_cfg.chroma_mode = awj::ChromaMode::auto_keep;
  alpha_cfg.bit_depth = {};
  alpha_cfg.alpha_policy = awj::AlphaModePolicy::automatic;
  awj::NativeBackend alpha_backend{
      alpha_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_result = alpha_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  const auto alpha_output = alpha_cfg.output_dir / "alpha-input.avif";
  if (!alpha_result.ok || !alpha_result.lossless ||
      alpha_result.final_encoder_quality != 100 ||
      alpha_result.encoder_id != "aom" || alpha_result.applied_alpha != "kept" ||
      alpha_result.applied_chroma != "444" || alpha_result.speed != 5 ||
      !std::filesystem::exists(alpha_output)) {
    return fail("transparent AVIF auto path did not force full AOM lossless while preserving speed.");
  }
  auto avif_decoder = awj::make_avif_image_decoder(1);
  auto alpha_decoded = avif_decoder->decode(alpha_output);
  if (!alpha_decoded ||
      !same_rgba(alpha_decoded->image, alpha_reference->image)) {
    return fail(alpha_decoded ? "transparent AVIF did not preserve full RGBA."
                              : alpha_decoded.error());
  }

  auto alpha_off_cfg = alpha_cfg;
  alpha_off_cfg.output_dir = output / "alpha-off";
  alpha_off_cfg.alpha_policy = awj::AlphaModePolicy::off;
  awj::NativeBackend alpha_off_backend{
      alpha_off_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_off_result = alpha_off_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  if (!alpha_off_result.ok || alpha_off_result.lossless ||
      alpha_off_result.final_encoder_quality != 70 ||
      alpha_off_result.applied_alpha != "stripped" ||
      alpha_off_result.speed != 5) {
    return fail("alpha=off no longer follows the requested lossy quality and speed.");
  }

  auto alpha_visual_cfg = alpha_cfg;
  alpha_visual_cfg.output_dir = output / "alpha-visual";
  alpha_visual_cfg.visual_quality = 80;
  awj::NativeBackend alpha_visual_backend{
      alpha_visual_cfg, logger,
      awj::ResourcePlan{.file_parallelism = 1,
                        .encoder_threads_per_file = 1,
                        .global_thread_budget = 1}};
  auto alpha_visual_result = alpha_visual_backend.encode(
      awj::ImageFile{.index = 0,
                     .path = alpha_input,
                     .bytes = std::filesystem::file_size(alpha_input)});
  if (!alpha_visual_result.ok || !alpha_visual_result.lossless ||
      alpha_visual_result.final_encoder_quality != 100 ||
      alpha_visual_result.search_attempt_count != 1 ||
      alpha_visual_result.applied_alpha != "kept" ||
      alpha_visual_result.applied_chroma != "444" ||
      alpha_visual_result.speed != 5) {
    return fail("transparent AVIF visual-quality path did not bypass to one lossless encode.");
  }

  cfg.visual_quality = 100;
  cfg.chroma_mode = awj::ChromaMode::yuv420;
  cfg.bit_depth = 8;
  awj::NativeBackend avif_visual_backend{cfg, logger, awj::ResourcePlan{
                                                          .file_parallelism = 1,
                                                          .encoder_threads_per_file = 1,
                                                          .global_thread_budget = 1}};
  auto avif_visual_result = avif_visual_backend.encode(awj::ImageFile{.index = 0,
                                                                      .path = input,
                                                                      .bytes = input_bytes});
  if (!avif_visual_result.ok || avif_visual_result.skipped || avif_visual_result.canceled) {
    return fail(avif_visual_result.message.empty() ? "native backend AVIF visual_quality encode failed."
                                                  : avif_visual_result.message);
  }
  if (avif_visual_result.search_attempt_count != 1 ||
      !avif_visual_result.lossless ||
      avif_visual_result.final_encoder_quality != 100 ||
      avif_visual_result.command.find("aom") == std::string::npos ||
      avif_visual_result.requested_encoder_id != "aom") {
    return fail(std::format(
        "native backend AVIF visual_quality did not use shared lossless auto search path: attempts={} lossless={} final_q={} command={} requested_encoder={}",
        avif_visual_result.search_attempt_count,
        avif_visual_result.lossless ? "true" : "false",
        avif_visual_result.final_encoder_quality,
        avif_visual_result.command,
        avif_visual_result.requested_encoder_id));
  }

  // 下一次启动会清理该目录；这里直接退出以避开第三方静态析构阶段的访问违规。
  terminate_test_process(0);
}
