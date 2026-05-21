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
                    "awjimage-cli-aom-test";
  const auto input = root / "input.webp";
  const auto output_dir = root / "out";
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
  cfg.output_dir = output_dir;
  cfg.output_format = awj::OutputFormat::avif;
  cfg.quality = 90;
  cfg.max_jobs = 1;
  cfg.avif_encoder = awj::AvifEncoderMode::automatic;
  cfg.chroma_mode = awj::ChromaMode::yuv444;
  cfg.bit_depth = 8;

  awj::FileLogger logger{output_dir, false};
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
    return fail(result.message.empty() ? "native AVIF AOM conversion failed."
                                      : result.message);
  }

  const auto output = output_dir / "input.avif";
  if (!std::filesystem::exists(output) || std::filesystem::file_size(output, ec) == 0 || ec) {
    return fail("native AVIF AOM did not create output.");
  }
  if (result.command.find("aom") == std::string::npos ||
      result.requested_encoder_id != "auto" || result.encoder_id != "aom") {
    return fail("native AVIF auto result did not record requested auto and applied AOM encoder.");
  }

  terminate_test_process(0);
}
