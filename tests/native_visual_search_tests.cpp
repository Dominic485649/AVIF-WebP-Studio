#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

import awj.codec;
import awj.config;
import awj.image;
import awj.native_visual_search;
import awj.resource_planner;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image() {
  awj::ImagePlane plane{.stride = 16};
  plane.bytes.resize(4 * 4 * 4);
  for (std::size_t y = 0; y < 4; ++y) {
    for (std::size_t x = 0; x < 4; ++x) {
      const auto offset = y * plane.stride + x * 4;
      plane.bytes[offset + 0] = std::byte{static_cast<unsigned char>(x * 60)};
      plane.bytes[offset + 1] = std::byte{static_cast<unsigned char>(y * 60)};
      plane.bytes[offset + 2] = std::byte{static_cast<unsigned char>((x + y) * 30)};
      plane.bytes[offset + 3] = std::byte{255};
    }
  }
  awj::ImageBuffer image{.width = 4,
                          .height = 4,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace

int main() {
  awj::WebPImageEncoder encoder;
  awj::WebPImageDecoder decoder;
  const auto candidate_path = std::filesystem::temp_directory_path() /
                              "awjimage-native-vq-test.webp";

  auto result = awj::encode_with_native_visual_quality_search(
      make_test_image(), encoder, decoder,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 40,
                                 .visual_quality = 30,
                                 .speed = 10,
                                 .visual_quality_fallback = true,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}},
      candidate_path);
  std::error_code ec;
  std::filesystem::remove(candidate_path, ec);
  if (!result) {
    return fail(result.error());
  }
  if (result->encode_result.encoded.bytes.empty() ||
      result->encode_result.search_attempt_count <= 1 ||
      !result->encode_result.visual_score ||
      result->encode_result.final_quality < 1 ||
      result->encode_result.final_quality > 99) {
    return fail("native visual quality search result invalid.");
  }
  if (result->encode_result.diagnostics.visual_quality_search_trace.find("predicted=q") ==
          std::string::npos ||
      result->encode_result.diagnostics.visual_quality_search_trace.find("selected=") ==
          std::string::npos) {
    return fail("native visual quality search did not record an explanatory trace.");
  }

  auto lossless = awj::encode_with_native_visual_quality_search(
      make_test_image(), encoder, decoder,
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                 .quality = 20,
                                 .visual_quality = 100,
                                 .speed = 10,
                                 .resources = awj::ResourcePlan{
                                     .file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}},
      candidate_path);
  std::filesystem::remove(candidate_path, ec);
  if (!lossless) {
    return fail(lossless.error());
  }
  if (!lossless->encode_result.lossless || lossless->encode_result.final_quality != 100 ||
      lossless->encode_result.search_attempt_count != 1) {
    return fail("native visual quality lossless path invalid.");
  }
  if (lossless->encode_result.diagnostics.visual_quality_search_trace.find("lossless-bypass") ==
      std::string::npos) {
    return fail("native visual quality lossless path did not record trace.");
  }

  return 0;
}
