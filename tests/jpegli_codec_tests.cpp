#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

import awj.codec;
import awj.config;
import awj.image;
import awj.jpegli_codec;
import awj.resource_planner;

namespace {

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

awj::ImageBuffer make_test_image(bool metadata) {
  awj::ImagePlane plane{.stride = 12};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
      std::byte{30},  std::byte{90},  std::byte{180}, std::byte{128},
      std::byte{240}, std::byte{200}, std::byte{80},  std::byte{255},
      std::byte{12},  std::byte{34},  std::byte{56},  std::byte{255},
      std::byte{78},  std::byte{90},  std::byte{12},  std::byte{255},
      std::byte{210}, std::byte{120}, std::byte{30},  std::byte{255},
  };
  awj::ImageBuffer image{.width = 3,
                         .height = 3,
                         .pixel_format = awj::PixelFormat::rgba,
                         .alpha_mode = awj::AlphaMode::straight,
                         .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  if (metadata) {
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::icc,
        .bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}});
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::exif,
        .bytes = {std::byte{'I'}, std::byte{'I'}, std::byte{42}, std::byte{0}}});
    image.metadata.push_back(awj::MetadataBlock{
        .kind = awj::MetadataKind::xmp,
        .bytes = {std::byte{'<'}, std::byte{'x'}, std::byte{'/'} }});
  }
  return image;
}

awj::NativeEncodeSettings jpegli_settings(int quality, bool strip_metadata) {
  return awj::NativeEncodeSettings{
      .output_format = awj::OutputFormat::jpgli,
      .quality = quality,
      .speed = awj::default_speed_for(awj::OutputFormat::jpgli),
      .source_has_icc = !strip_metadata,
      .applied_icc = strip_metadata ? "stripped" : "kept",
      .strip_metadata = strip_metadata,
      .resources = awj::ResourcePlan{.file_parallelism = 1,
                                     .encoder_threads_per_file = 1,
                                     .global_thread_budget = 1}};
}

bool has_metadata_kind(const awj::ImageBuffer& image, awj::MetadataKind kind) {
  return std::ranges::any_of(image.metadata, [kind](const awj::MetadataBlock& block) {
    return block.kind == kind && !block.bytes.empty();
  });
}

}  // namespace

int main() {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

  awj::JpegliImageEncoder encoder;
  auto encoded = encoder.encode(make_test_image(true), jpegli_settings(42, false));
  if (!encoded) {
    return fail(encoded.error());
  }
  if (encoded->encoded.bytes.empty() || encoded->encoded.codec_name != "jpegli") {
    return fail("JPGLI encode result invalid.");
  }
  if (encoded->final_quality != 42 ||
      encoded->diagnostics.encoder_id != "jpegli" ||
      !encoded->diagnostics.speed_mapping.codec_key.empty() ||
      encoded->diagnostics.speed_mapping.codec_value != 0) {
    return fail("JPGLI quality or diagnostics invalid.");
  }

  awj::JpegliImageDecoder decoder;
  auto memory_decoded = decoder.decode_memory(
      std::span<const std::byte>{encoded->encoded.bytes}, "jpglI-memory-test");
  if (!memory_decoded) {
    return fail(memory_decoded.error());
  }
  if (memory_decoded->decoder_id != "jpegli" ||
      memory_decoded->image.width != 3 ||
      memory_decoded->image.height != 3 ||
      memory_decoded->image.pixel_format != awj::PixelFormat::rgba ||
      memory_decoded->image.planes.empty()) {
    return fail("JPGLI decode_memory result invalid.");
  }
  if (!has_metadata_kind(memory_decoded->image, awj::MetadataKind::icc) ||
      !has_metadata_kind(memory_decoded->image, awj::MetadataKind::exif) ||
      !has_metadata_kind(memory_decoded->image, awj::MetadataKind::xmp)) {
    return fail("JPGLI metadata was not preserved.");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "awjimage-jpegli-codec-test.jpg";
  {
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
                 static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  }

  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions || dimensions->width != 3 || dimensions->height != 3) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return fail(dimensions ? "JPGLI probe dimensions invalid." : dimensions.error());
  }

  auto file_decoded = decoder.decode(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!file_decoded || file_decoded->decoder_id != "jpegli") {
    return fail(file_decoded ? "JPGLI file decode id invalid." : file_decoded.error());
  }

  auto stripped = encoder.encode(make_test_image(true), jpegli_settings(90, true));
  if (!stripped) {
    return fail(stripped.error());
  }
  auto stripped_decoded = decoder.decode_memory(
      std::span<const std::byte>{stripped->encoded.bytes}, "jpglI-strip-test");
  if (!stripped_decoded) {
    return fail(stripped_decoded.error());
  }
  if (!stripped_decoded->image.metadata.empty()) {
    return fail("JPGLI strip metadata did not remove APP/ICC metadata.");
  }

  auto advanced_settings = jpegli_settings(80, true);
  advanced_settings.requested_chroma_mode = awj::ChromaMode::yuv420;
  advanced_settings.chroma_mode = awj::ChromaMode::yuv420;
  advanced_settings.requested_bit_depth = 8;
  advanced_settings.bit_depth = 8;
  advanced_settings.jpegli_progressive_level = 0;
  advanced_settings.jpegli_optimize_huffman = false;
  auto advanced = encoder.encode(make_test_image(false), advanced_settings);
  if (!advanced) {
    return fail(advanced.error());
  }
  auto advanced_decoded = decoder.decode_memory(
      std::span<const std::byte>{advanced->encoded.bytes}, "jpglI-advanced-test");
  if (!advanced_decoded || advanced_decoded->image.width != 3 ||
      advanced_decoded->image.height != 3) {
    return fail(advanced_decoded ? "JPGLI advanced decode dimensions invalid."
                                 : advanced_decoded.error());
  }

  auto invalid_settings = jpegli_settings(80, true);
  invalid_settings.jpegli_progressive_level = 2;
  invalid_settings.jpegli_optimize_huffman = false;
  auto invalid = encoder.encode(make_test_image(false), invalid_settings);
  if (invalid ||
      invalid.error().find("渐进 JPEG 需要优化哈夫曼表") == std::string::npos) {
    return fail("JPGLI invalid progressive/fixed Huffman combination was not rejected.");
  }

  terminate_test_process(0);
}
