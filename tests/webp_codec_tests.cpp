#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

#include <webp/encode.h>
#include <webp/mux.h>

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

struct AnimEncoderDeleter {
  void operator()(WebPAnimEncoder* value) const noexcept {
    if (value != nullptr) WebPAnimEncoderDelete(value);
  }
};

bool add_animation_frame(WebPAnimEncoder* encoder, const std::uint8_t* rgba,
                         int timestamp, const WebPConfig& config) {
  WebPPicture picture{};
  if (!WebPPictureInit(&picture)) return false;
  picture.use_argb = 1;
  picture.width = 2;
  picture.height = 2;
  const bool ok = WebPPictureImportRGBA(&picture, rgba, 8) &&
                  WebPAnimEncoderAdd(encoder, &picture, timestamp, &config);
  WebPPictureFree(&picture);
  return ok;
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

  WebPAnimEncoderOptions anim_options{};
  WebPConfig anim_config{};
  if (!WebPAnimEncoderOptionsInit(&anim_options) ||
      !WebPConfigInit(&anim_config)) {
    return fail("WebP animation test initialization failed.");
  }
  anim_config.lossless = 1;
  anim_config.quality = 100.0f;
  std::unique_ptr<WebPAnimEncoder, AnimEncoderDeleter> anim{
      WebPAnimEncoderNew(2, 2, &anim_options)};
  const std::uint8_t red[16]{255, 0, 0, 255, 255, 0, 0, 255,
                             255, 0, 0, 255, 255, 0, 0, 255};
  const std::uint8_t blue[16]{0, 0, 255, 255, 0, 0, 255, 255,
                              0, 0, 255, 255, 0, 0, 255, 255};
  WebPData animation{};
  if (!anim || !add_animation_frame(anim.get(), red, 0, anim_config) ||
      !add_animation_frame(anim.get(), blue, 100, anim_config) ||
      !WebPAnimEncoderAdd(anim.get(), nullptr, 200, nullptr) ||
      !WebPAnimEncoderAssemble(anim.get(), &animation)) {
    return fail("WebP animation test encoding failed.");
  }
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(animation.bytes),
                 static_cast<std::streamsize>(animation.size));
  }
  WebPDataClear(&animation);
  decoded = decoder.decode(path);
  std::filesystem::remove(path, ec);
  if (!decoded || decoded->image.planes.empty() ||
      decoded->image.planes[0].bytes.size() < 4 ||
      decoded->image.planes[0].bytes[0] != std::byte{255} ||
      decoded->image.planes[0].bytes[1] != std::byte{0} ||
      decoded->image.planes[0].bytes[2] != std::byte{0}) {
    return fail(decoded ? "WebP animation did not flatten to its first frame."
                        : decoded.error());
  }

  return 0;
}
