#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

import awj.avif_aom_codec;
import awj.codec;
import awj.config;
import awj.image;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
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

awj::ImageBuffer make_grid_test_image(std::uint32_t width = 128,
                                      std::uint32_t height = 128) {
  awj::ImagePlane plane{.stride = width * 4};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((x * 2) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((y * 2) & 0xffu)});
      plane.bytes.push_back(std::byte{static_cast<unsigned char>((x + y) & 0xffu)});
      plane.bytes.push_back(std::byte{255});
    }
  }
  awj::ImageBuffer image{.width = width,
                          .height = height,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::none,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

awj::NativeEncodeSettings settings(int bit_depth,
                                    awj::ChromaMode chroma,
                                    awj::AvifEncoderMode encoder) {
  return awj::NativeEncodeSettings{.output_format = awj::OutputFormat::avif,
                                    .quality = 80,
                                    .speed = 6,
                                    .speed_explicit = true,
                                    .bit_depth = bit_depth,
                                    .chroma_mode = chroma,
                                    .avif_encoder = encoder,
                                    .resources = awj::ResourcePlan{
                                        .file_parallelism = 1,
                                        .encoder_threads_per_file = 1,
                                        .global_thread_budget = 1}};
}

int decode_bytes_to_temp(const awj::EncodedImage& encoded,
                         const std::filesystem::path& temp,
                         std::uint32_t expected_width,
                         std::uint32_t expected_height,
                         int expected_rgba_bit_depth,
                         int expected_source_bit_depth) {
  {
    std::ofstream output{temp, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded.bytes.size()));
  }
  auto decoder = awj::make_avif_image_decoder(1);
  auto decoded = decoder->decode(temp);
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (!decoded || decoded->image.width != expected_width ||
      decoded->image.height != expected_height ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.bit_depth != expected_rgba_bit_depth ||
      !decoded->image.source_info ||
      decoded->image.source_info->bit_depth != expected_source_bit_depth) {
    if (decoded) {
      return fail(std::format("AVIF decode result invalid: got {}x{} {}-bit source {}-bit.",
                              decoded->image.width, decoded->image.height,
                              decoded->image.bit_depth,
                              decoded->image.source_info
                                  ? decoded->image.source_info->bit_depth
                                  : 0));
    }
    return fail(decoded.error());
  }
  return 0;
}

}  // namespace

int main() {
  auto aom = awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
  auto encoded = aom->encode(make_test_image(), settings(8, awj::ChromaMode::yuv444,
                                                         awj::AvifEncoderMode::aom));
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->encoded.bytes.empty() || encoded->encoded.codec_name != "libavif-aom") {
    return fail("AVIF AOM encoder did not produce bytes.");
  }
  if (encoded->diagnostics.encoder_id != "aom" ||
      encoded->diagnostics.applied_chroma != "444" ||
      encoded->diagnostics.speed_mapping.codec_key != "aom:cpu-used" ||
      encoded->diagnostics.speed_mapping.codec_value != 6) {
    return fail("AVIF AOM diagnostics invalid.");
  }

  auto default_speed_settings = settings(8, awj::ChromaMode::yuv420,
                                         awj::AvifEncoderMode::aom);
  default_speed_settings.speed_explicit = false;
  auto default_speed = aom->encode(make_test_image(), default_speed_settings);
  if (!default_speed || default_speed->diagnostics.speed_mapping.codec_key != "aom:cpu-used" ||
      default_speed->diagnostics.speed_mapping.codec_value != 6) {
    return fail(default_speed ? "AOM default speed diagnostics invalid."
                              : default_speed.error());
  }

  auto ten_bit = aom->encode(make_test_image(), settings(10, awj::ChromaMode::yuv420,
                                                         awj::AvifEncoderMode::aom));
  if (!ten_bit || ten_bit->encoded.bytes.empty() ||
      ten_bit->diagnostics.applied_bit_depth != 10) {
    return fail(ten_bit ? "AOM 10-bit diagnostics invalid." : ten_bit.error());
  }
  auto twelve_bit = aom->encode(make_test_image(), settings(12, awj::ChromaMode::yuv420,
                                                            awj::AvifEncoderMode::aom));
  if (!twelve_bit || twelve_bit->encoded.bytes.empty() ||
      twelve_bit->diagnostics.applied_bit_depth != 12) {
    return fail(twelve_bit ? "AOM 12-bit diagnostics invalid." : twelve_bit.error());
  }

  auto grid_settings = settings(8, awj::ChromaMode::yuv420,
                                awj::AvifEncoderMode::aom);
  grid_settings.resources = awj::ResourcePlan{.file_parallelism = 2,
                                              .encoder_threads_per_file = 4,
                                              .global_thread_budget = 4};
  grid_settings.alpha_policy = awj::AlphaModePolicy::off;
  grid_settings.avif_grid_plan = awj::GridPlan{.cols = 2,
                                               .rows = 2,
                                               .tile_width = 64,
                                               .tile_height = 64,
                                               .padded_width = 128,
                                               .padded_height = 128,
                                               .uses_padding = false,
                                               .clamped_to_original_size = false};
  auto grid_encoded = aom->encode(make_grid_test_image(), grid_settings);
  if (!grid_encoded || grid_encoded->encoded.bytes.empty() ||
      grid_encoded->diagnostics.integration_mode != "libavif-grid" ||
      grid_encoded->diagnostics.encoder_threads != 4) {
    return fail(grid_encoded ? "AOM grid diagnostics invalid." : grid_encoded.error());
  }

  auto edge_grid_settings = settings(8, awj::ChromaMode::auto_keep,
                                     awj::AvifEncoderMode::aom);
  edge_grid_settings.alpha_policy = awj::AlphaModePolicy::off;
  edge_grid_settings.avif_grid_plan = awj::GridPlan{
      .cols = 2, .rows = 2, .tile_width = 65, .tile_height = 64,
      .padded_width = 130, .padded_height = 128, .uses_padding = true,
      .clamped_to_original_size = true};
  auto edge_grid_encoded = aom->encode(make_grid_test_image(129, 127),
                                       edge_grid_settings);
  if (!edge_grid_encoded || edge_grid_encoded->encoded.bytes.empty() ||
      edge_grid_encoded->diagnostics.applied_chroma != "444") {
    return fail(edge_grid_encoded ? "AOM edge grid diagnostics invalid."
                                  : edge_grid_encoded.error());
  }

  auto zen = awj::make_avif_image_encoder(awj::AvifEncoderMode::zenrav1e);
  auto zen_encoded = zen->encode(make_test_image(), settings(10, awj::ChromaMode::yuv444,
                                                             awj::AvifEncoderMode::zenrav1e));
  if (awj::avif_zenravif_encoder_available()) {
    if (!zen_encoded || zen_encoded->encoded.bytes.empty() ||
        zen_encoded->encoded.codec_name != "zenravif") {
      return fail(zen_encoded ? "zenravif did not produce bytes." : zen_encoded.error());
    }
  } else if (zen_encoded || zen_encoded.error().find("not available") == std::string::npos) {
    return fail("zenravif unavailable build did not report clear error.");
  }

  const auto temp_dir = std::filesystem::temp_directory_path();
  if (const int rc = decode_bytes_to_temp(encoded->encoded,
                                          temp_dir / "avif-aom-codec-test.avif",
                                          2, 2, 8, 8);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(ten_bit->encoded,
                                          temp_dir / "avif-aom-codec-test-10.avif",
                                          2, 2, 16, 10);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(twelve_bit->encoded,
                                          temp_dir / "avif-aom-codec-test-12.avif",
                                          2, 2, 16, 12);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(grid_encoded->encoded,
                                          temp_dir / "avif-aom-codec-test-grid.avif",
                                          128, 128, 8, 8);
      rc != 0) {
    return rc;
  }
  if (const int rc = decode_bytes_to_temp(
          edge_grid_encoded->encoded,
          temp_dir / "avif-aom-codec-test-edge-grid.avif", 129, 127, 8, 8);
      rc != 0) {
    return rc;
  }
  return 0;
}
