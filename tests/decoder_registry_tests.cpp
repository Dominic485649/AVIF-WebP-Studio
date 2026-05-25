#include <filesystem>
#include <iostream>

import awj.decoder_registry;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  auto png = awj::select_decoder_for_path("sample.png", {.allow_wic_fallback = true});
  if (!png || png->fallback || png->decoder->id() != "libpng") {
    return fail("PNG did not select libpng before WIC.");
  }

  auto jpeg = awj::select_decoder_for_path("sample.jpeg", {.allow_wic_fallback = true});
  if (!jpeg || jpeg->fallback || jpeg->decoder->id() != "libjpeg-turbo") {
    return fail("JPEG did not select libjpeg-turbo before WIC.");
  }

  auto gif = awj::select_decoder_for_path("sample.gif", {.allow_wic_fallback = true});
  if (!gif || gif->fallback || gif->decoder->id() != "giflib") {
    return fail("GIF did not select giflib before WIC.");
  }

  auto tiff = awj::select_decoder_for_path("sample.tiff", {.allow_wic_fallback = true});
  if (!tiff || tiff->fallback || tiff->decoder->id() != "libtiff") {
    return fail("TIFF did not select libtiff before WIC.");
  }

  auto raw = awj::select_decoder_for_path("sample.awsraw", {.allow_wic_fallback = true});
  if (!raw || raw->fallback || raw->decoder->id() != "awj-raw") {
    return fail("AWJ raw did not select internal raw decoder.");
  }

  auto camera_raw = awj::select_decoder_for_path("sample.dng", {.allow_wic_fallback = true});
  if (!camera_raw || camera_raw->fallback || camera_raw->decoder->id() != "libraw") {
    return fail("Camera RAW did not select LibRaw before WIC.");
  }

  auto heif = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = true});
  if (!heif || !heif->fallback || heif->decoder->id() != "wic") {
    return fail("HEIF did not route to WIC fallback when enabled.");
  }

  auto disabled = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = false});
  if (disabled) {
    return fail("WIC-only format was accepted while WIC fallback was disabled.");
  }

  return 0;
}
