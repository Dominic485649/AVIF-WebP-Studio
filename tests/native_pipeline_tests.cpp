#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <windows.h>

import awj.config;
import awj.core;
import awj.image;
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
      avif_visual_result.requested_encoder_id != "auto") {
    return fail("native backend AVIF visual_quality did not use shared lossless auto search path.");
  }

  // 下一次启动会清理该目录；这里直接退出以避开第三方静态析构阶段的访问违规。
  terminate_test_process(0);
}
