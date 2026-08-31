#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

import awj.codec;
import awj.config;
import awj.image;
import awj.png_codec;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::byte>& bytes) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

}  // namespace

int main() {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{128},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{64},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{0},
  };
  awj::ImageBuffer image{.width = 2,
                          .height = 2,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));

  awj::PngImageEncoder encoder;
  const auto capabilities = encoder.capabilities();
  if (capabilities.min_quality != 100 || capabilities.max_quality != 100) {
    return fail("PNG encoder quality contract must remain fixed at 100.");
  }

  // PNG 编码路径本身必须保持无损；即使通用质量字段传入较低值，也不能静默变成有损编码。
  auto encoded = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::png,
                                 .quality = 1,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  if (!encoded->lossless || encoded->encoded.bytes.empty()) {
    return fail("PNG lossless encode result invalid.");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-png-codec-test.png";
  if (!write_file(path, encoded->encoded.bytes)) {
    return fail("Could not write temporary PNG test input.");
  }

  awj::PngImageDecoder decoder;
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded) {
    return fail(decoded.error());
  }
  if (decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.alpha_mode != awj::AlphaMode::straight ||
      decoded->image.bit_depth != 8 || !decoded->image.source_info ||
      decoded->image.source_info->bit_depth != 8 || decoded->image.planes.empty()) {
    return fail("PNG decode result invalid.");
  }
  const auto& decoded_plane = decoded->image.planes.front();
  if (decoded_plane.bytes.size() < 16 || decoded_plane.bytes[3] != std::byte{255} ||
      decoded_plane.bytes[7] != std::byte{128} ||
      decoded_plane.bytes[11] != std::byte{64} ||
      decoded_plane.bytes[15] != std::byte{0}) {
    return fail("PNG alpha channel was not preserved.");
  }

  return 0;
}
