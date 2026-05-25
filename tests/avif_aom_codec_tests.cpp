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

}  // namespace

int main() {
  awj::AvifAomImageEncoder aom;
  auto encoded = aom.encode(make_test_image(), settings(8, awj::ChromaMode::yuv444,
                                                        awj::AvifEncoderMode::aom));
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->encoded.bytes.empty() || encoded->encoded.codec_name != "libavif-aom") {
    return fail("AVIF AOM encoder did not produce bytes.");
  }
  if (encoded->diagnostics.encoder_id != "aom" ||
      encoded->diagnostics.applied_chroma != "444" ||
      encoded->diagnostics.speed_mapping.codec_key != "aom:cpu-used") {
    return fail("AVIF AOM diagnostics invalid.");
  }

  auto ten_bit = aom.encode(make_test_image(), settings(10, awj::ChromaMode::yuv420,
                                                        awj::AvifEncoderMode::aom));
  if (!ten_bit || ten_bit->encoded.bytes.empty() ||
      ten_bit->diagnostics.applied_bit_depth != 10) {
    return fail(ten_bit ? "AOM 10-bit diagnostics invalid." : ten_bit.error());
  }
  auto twelve_bit = aom.encode(make_test_image(), settings(12, awj::ChromaMode::yuv420,
                                                           awj::AvifEncoderMode::aom));
  if (!twelve_bit || twelve_bit->encoded.bytes.empty() ||
      twelve_bit->diagnostics.applied_bit_depth != 12) {
    return fail(twelve_bit ? "AOM 12-bit diagnostics invalid." : twelve_bit.error());
  }

  awj::ZenravifImageEncoder zen;
  auto zen_encoded = zen.encode(make_test_image(), settings(10, awj::ChromaMode::yuv444,
                                                            awj::AvifEncoderMode::zenrav1e));
  if (awj::avif_zenravif_encoder_available()) {
    if (!zen_encoded || zen_encoded->encoded.bytes.empty() ||
        zen_encoded->encoded.codec_name != "zenravif") {
      return fail(zen_encoded ? "zenravif did not produce bytes." : zen_encoded.error());
    }
  } else if (zen_encoded || zen_encoded.error().find("not available") == std::string::npos) {
    return fail("zenravif unavailable build did not report clear error.");
  }

  const auto temp = std::filesystem::temp_directory_path() / "avif-aom-codec-test.avif";
  {
    std::ofstream output{temp, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }
  awj::AvifImageDecoder decoder;
  auto decoded = decoder.decode(temp);
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (!decoded || decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.pixel_format != awj::PixelFormat::rgba ||
      decoded->image.bit_depth != 8) {
    return fail(decoded ? "AVIF decode result invalid." : decoded.error());
  }
  return 0;
}
