#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

import awj.codec;
import awj.config;
import awj.image;
import awj.resource_planner;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

}  // namespace

int main() {
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

  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      image,
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
  if (!encoded->lossless || encoded->encoded.bytes.empty()) {
    return fail("WebP lossless encode result invalid.");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-webp-codec-test.webp";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::WebPImageDecoder decoder;
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded) {
    return fail(decoded.error());
  }
  if (decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.planes.empty()) {
    return fail("WebP decode result invalid.");
  }

  return 0;
}
