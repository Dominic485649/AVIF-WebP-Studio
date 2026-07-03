#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

void push_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffu)});
  bytes.push_back(std::byte{static_cast<unsigned char>(value >> 8)});
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

  awj::JXLImageEncoder encoder;
  auto encoded = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!encoded) {
    return fail(encoded.error());
  }
  if (!encoded->lossless || encoded->encoded.bytes.empty()) {
    return fail("JXL lossless encode result invalid.");
  }
  if (encoded->diagnostics.speed_mapping.codec_value != 1) {
    return fail("JXL speed mapping invalid.");
  }

  auto default_speed = encoder.encode(
      image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 90,
                                 .speed = awj::default_speed_for(awj::OutputFormat::jxl),
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!default_speed || default_speed->diagnostics.speed_mapping.codec_value != awj::encoding_defaults::default_jxl_effort) {
    return fail(default_speed ? "JXL default effort diagnostics invalid." : default_speed.error());
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-jxl-codec-test.jxl";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  awj::JXLImageDecoder decoder;
  auto decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!decoded) {
    return fail(decoded.error());
  }
  if (decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.planes.empty()) {
    return fail("JXL decode result invalid.");
  }

  awj::ImagePlane hdr_plane{.stride = 16};
  for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{32768},
                                    std::uint16_t{65535}, std::uint16_t{65535},
                                    std::uint16_t{65535}, std::uint16_t{0},
                                    std::uint16_t{32768}, std::uint16_t{65535}}) {
    push_u16(hdr_plane.bytes, value);
  }
  awj::ImageBuffer hdr_image{.width = 2,
                             .height = 1,
                             .pixel_format = awj::PixelFormat::rgba,
                             .alpha_mode = awj::AlphaMode::straight,
                             .bit_depth = 16};
  hdr_image.planes.push_back(std::move(hdr_plane));
  auto hdr_encoded = encoder.encode(
      hdr_image,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::jxl,
                                 .quality = 100,
                                 .speed = 10,
                                 .speed_explicit = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}});
  if (!hdr_encoded || hdr_encoded->encoded.bytes.empty()) {
    return fail(hdr_encoded ? "JXL 16-bit encode result invalid." : hdr_encoded.error());
  }

  const auto hdr_path = std::filesystem::temp_directory_path() /
                        "awjimage-jxl-codec-test-16.jxl";
  {
    std::ofstream output{hdr_path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(hdr_encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(hdr_encoded->encoded.bytes.size()));
  }
  auto hdr_decoded = decoder.decode(hdr_path);
  std::filesystem::remove(hdr_path, ec);
  if (!hdr_decoded || hdr_decoded->image.bit_depth != 16 ||
      !hdr_decoded->image.source_info ||
      hdr_decoded->image.source_info->bit_depth != 16 ||
      !hdr_decoded->image.source_info->has_hdr_metadata) {
    return fail(hdr_decoded ? "JXL 16-bit decode result invalid." : hdr_decoded.error());
  }

  return 0;
}
