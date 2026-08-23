#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>

import awj.image;
import awj.raw_image_io;

namespace {

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  std::array<std::uint16_t, 4> pixel{0x3800u, 0xb400u, 0x4000u, 0x3c00u};
  std::string payload(sizeof(pixel), '\0');
  std::memcpy(payload.data(), pixel.data(), payload.size());
  std::istringstream input{payload, std::ios::binary};
  const auto path = std::filesystem::temp_directory_path() /
                    ("awj-wgc-stdin-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     ".awsraw");
  const auto cleanup = [&] {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  };
  if (auto written = awj::write_wgc_scrgb_half_stream_file(path, 1, 1, input);
      !written) {
    cleanup();
    return fail(written.error());
  }
  const auto image = awj::read_raw_image_file(path);
  cleanup();
  if (!image || image->width != 1 || image->height != 1 ||
      image->bit_depth != 16 ||
      image->sample_representation != awj::SampleRepresentation::ieee_half_float ||
      !image->source_info ||
      image->source_info->color_metadata_source != "wgc-scrgb-half-linear" ||
      image->planes.size() != 1 || image->planes.front().bytes.size() != payload.size()) {
    return fail(image ? "WGC stdin raw metadata was not preserved" : image.error());
  }
  std::istringstream short_input{"\0\0", std::ios::binary};
  if (awj::write_wgc_scrgb_half_stream_file(path, 1, 1, short_input)) {
    cleanup();
    return fail("short WGC stdin frame was accepted");
  }
  cleanup();
  std::cout << "WGC stdin raw tests passed\n";
  return 0;
}
