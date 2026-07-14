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
  if (!jpeg || jpeg->fallback) {
    return fail("JPEG did not select a native decoder before WIC.");
  }
#if AWJ_HAS_JPEGLI
  if (jpeg->decoder->id() != "jpegli") {
    return fail("JPEG did not select Jpegli before libjpeg-turbo/WIC.");
  }
#else
  if (jpeg->decoder->id() != "libjpeg-turbo") {
    return fail("JPEG did not select libjpeg-turbo before WIC.");
  }
#endif
  auto jpeg_without_wic = awj::select_decoder_for_path("sample.jfif",
                                                       {.allow_wic_fallback = false});
  if (!jpeg_without_wic || jpeg_without_wic->fallback) {
    return fail("JPEG series should be native with WIC fallback disabled.");
  }

  for (const char* path : {"sample.bmp", "sample.dib", "sample.rle"}) {
    auto bmp = awj::select_decoder_for_path(path, {.allow_wic_fallback = false});
    if (!bmp || bmp->fallback || bmp->decoder->id() != "awj-bmp") {
      return fail("BMP series did not select the native BMP decoder.");
    }
  }

  for (const char* path : {"sample.jxr", "sample.wdp", "sample.hdp"}) {
    auto jxr = awj::select_decoder_for_path(path, {.allow_wic_fallback = false});
#if AWJ_HAS_WINDOWS_CODECS
    if (!jxr || jxr->fallback || jxr->decoder->id() != "windows-jxr") {
      return fail("JXR series did not select the Windows native JXR decoder.");
    }
#else
    if (jxr) {
      return fail("Linux unexpectedly registered the Windows JXR decoder.");
    }
#endif
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
#if AWJ_HAS_AWJ_RAW_CODEC
  if (!raw || raw->fallback || raw->decoder->id() != "awj-raw") {
    return fail("AWJ raw did not select internal raw decoder.");
  }
#else
  if (raw) {
    return fail("Build without AWJ raw support unexpectedly registered its decoder.");
  }
#endif

  auto camera_raw = awj::select_decoder_for_path("sample.dng", {.allow_wic_fallback = true});
  if (!camera_raw || camera_raw->fallback || camera_raw->decoder->id() != "libraw") {
    return fail("Camera RAW did not select LibRaw before WIC.");
  }

  auto heif = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = true});
#if AWJ_HAS_WINDOWS_CODECS
  if (!heif || !heif->fallback || heif->decoder->id() != "wic") {
    return fail("HEIF did not route to WIC fallback when enabled.");
  }
#else
  if (heif) {
    return fail("Linux unexpectedly accepted HEIF through WIC fallback.");
  }
#endif

  auto ico = awj::select_decoder_for_path("sample.ico", {.allow_wic_fallback = true});
#if AWJ_HAS_WINDOWS_CODECS
  if (!ico || !ico->fallback || ico->decoder->id() != "wic") {
    return fail("ICO did not route to WIC fallback when enabled.");
  }
#else
  if (ico) {
    return fail("Linux unexpectedly accepted ICO through WIC fallback.");
  }
#endif

  auto disabled = awj::select_decoder_for_path("sample.heif", {.allow_wic_fallback = false});
  if (disabled) {
    return fail("WIC-only format was accepted while WIC fallback was disabled.");
  }

  return 0;
}
