#include <format>
#include <iostream>
#include <string>
#include <vector>

import awj.config;
import awj.codec;
import awj.encoding_defaults;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  const auto defaults = awj::default_app_config();
  if (defaults.input_path.native() !=
      std::wstring{awj::encoding_defaults::default_input_path}) {
    return fail("default input path does not come from encoding defaults.");
  }
  if (defaults.output_template !=
      awj::encoding_defaults::default_output_template) {
    return fail(
        "default output template does not come from encoding defaults.");
  }
  if (defaults.quality != awj::encoding_defaults::default_avif_quality) {
    return fail("default AVIF quality mismatch.");
  }
  if (awj::encoding_defaults::default_avif_quality != 70 ||
      awj::encoding_defaults::default_jxl_quality != 85) {
    return fail("AVIF/JXL default quality literals mismatch.");
  }
  if (!defaults.allow_wic_fallback) {
    return fail("default WIC fallback should be enabled.");
  }
  if (!defaults.enable_experimental_encoders) {
    return fail("experimental encoders should be enabled by default.");
  }
  if (defaults.memory_limit_bytes !=
      awj::encoding_defaults::default_memory_limit_bytes) {
    return fail("default memory limit mismatch.");
  }
  if (defaults.encode_timeout_minutes !=
      awj::encoding_defaults::preset_balanced_timeout_minutes) {
    return fail("default encode timeout mismatch.");
  }

  if (awj::default_quality_for(awj::OutputFormat::avif) !=
          awj::encoding_defaults::default_avif_quality ||
      awj::default_quality_for(awj::OutputFormat::webp) !=
          awj::encoding_defaults::default_webp_quality ||
      awj::default_quality_for(awj::OutputFormat::jxl) !=
          awj::encoding_defaults::default_jxl_quality ||
      awj::default_quality_for(awj::OutputFormat::jpgli) !=
          awj::encoding_defaults::default_jpegli_quality) {
    return fail("format quality defaults mismatch.");
  }

  if (awj::default_speed_for(awj::OutputFormat::webp) !=
          awj::encoding_defaults::default_webp_native_speed ||
      awj::default_speed_for(awj::OutputFormat::avif) !=
          awj::encoding_defaults::default_avif_native_speed ||
      awj::default_speed_for(awj::OutputFormat::jxl) !=
          awj::encoding_defaults::default_jxl_native_speed ||
      awj::default_speed_for(awj::OutputFormat::jpgli) !=
          awj::encoding_defaults::default_jpegli_native_speed) {
    return fail("format speed defaults mismatch.");
  }
  if (awj::encoding_defaults::default_webp_native_speed != 4 ||
      awj::encoding_defaults::default_jxl_native_speed != 6) {
    return fail("format speed default literals mismatch.");
  }
  if (awj::map_jxl_speed_to_effort(0).codec_value != 10 ||
      awj::map_jxl_speed_to_effort(6).codec_value != 4 ||
      awj::map_jxl_speed_to_effort(10).codec_value != 1) {
    return fail("JXL speed to effort mapping mismatch.");
  }

  auto webp_cfg = awj::default_app_config();
  awj::apply_format_defaults(webp_cfg, awj::OutputFormat::webp);
  if (webp_cfg.output_format != awj::OutputFormat::webp ||
      webp_cfg.quality != awj::encoding_defaults::default_webp_quality) {
    return fail("format defaults did not apply WebP quality.");
  }

  auto parsed = awj::parse_arguments({L"--format", L"jxl"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != awj::encoding_defaults::default_jxl_quality) {
    return fail("CLI JXL default quality mismatch.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli ||
      parsed->config.quality != awj::encoding_defaults::default_jpegli_quality) {
    return fail("CLI JPGLI format alias did not parse.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpegli"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli) {
    return fail("CLI Jpegli format alias did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--format", L"jpgli", L"--chroma", L"422",
       L"--jpegli-progressive-level", L"0", L"--jpegli-fixed-huffman",
       L"--jpegli-xyb"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli ||
      parsed->config.chroma_mode != awj::ChromaMode::yuv422 ||
      parsed->config.jpegli_progressive_level != 0 ||
      parsed->config.jpegli_optimize_huffman ||
      !parsed->config.jpegli_xyb) {
    return fail("CLI JPGLI advanced options did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--format", L"jpgli", L"--jpegli-progressive-level", L"2",
       L"--jpegli-fixed-huffman"});
  if (parsed ||
      parsed.error().find("渐进 JPEG 需要优化哈夫曼表") == std::string::npos) {
    return fail("CLI JPGLI invalid progressive/fixed huffman combination was not rejected.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpg"});
  if (parsed || parsed.error().find("输出格式不支持") == std::string::npos) {
    return fail("CLI should not accept plain jpg as the JPGLI entry.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli", L"--alpha", L"force"});
  if (parsed ||
      parsed.error().find("JPGLI 不支持 alpha 输出") == std::string::npos) {
    return fail("CLI did not reject forced alpha for JPGLI.");
  }

  parsed = awj::parse_arguments({L"--format", L"webp", L"--quality", L"82"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != 82) {
    return fail("explicit CLI quality was overwritten by defaults.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli", L"--speed", L"6"});
  if (parsed || parsed.error().find("JPGLI 不支持 --speed") == std::string::npos) {
    return fail("CLI should reject speed for JPGLI.");
  }
  parsed = awj::parse_arguments({L"--format", L"png", L"--speed", L"6"});
  if (parsed || parsed.error().find("PNG 不支持 --speed") == std::string::npos) {
    return fail("CLI should reject speed for PNG.");
  }

  parsed = awj::parse_arguments({L"--shell-convert", L"--format", L"png",
                                 L"C:\\img\\a.webp", L"C:\\img\\b.webp",
                                 L"C:\\img\\a.webp"});
  if (!parsed || parsed->config.output_policy != awj::OutputPolicy::shell ||
      parsed->shell_inputs.size() != 2 ||
      parsed->shell_inputs[0].native() != L"C:\\img\\a.webp" ||
      parsed->shell_inputs[1].native() != L"C:\\img\\b.webp") {
    return fail("CLI shell conversion inputs were not parsed and de-duplicated.");
  }

  parsed = awj::parse_arguments({L"--collision", L"number"});
  if (!parsed || parsed->config.collision_mode != awj::CollisionMode::suffix_number) {
    return fail("CLI numbered collision mode did not parse.");
  }

  parsed = awj::parse_arguments({L"--image-size-limit", L"manual", L"--max-width", L"1600",
                                 L"--max-height", L"1200", L"--max-long-edge", L"2000",
                                 L"--max-short-edge", L"900"});
  if (!parsed || parsed->config.image_size_limit.mode != awj::ImageSizeLimitMode::manual ||
      parsed->config.image_size_limit.max_width.value_or(0) != 1600 ||
      parsed->config.image_size_limit.max_height.value_or(0) != 1200 ||
      parsed->config.image_size_limit.max_long_edge.value_or(0) != 2000 ||
      parsed->config.image_size_limit.max_short_edge.value_or(0) != 900) {
    return fail("CLI image size limit did not parse.");
  }

  parsed = awj::parse_arguments({L"--suffix-number"});
  if (!parsed || parsed->config.collision_mode != awj::CollisionMode::suffix_number) {
    return fail("CLI --suffix-number did not parse.");
  }

  parsed = awj::parse_arguments({L"C:\\img\\a.webp"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("normal CLI mode should still reject positional inputs.");
  }

  parsed = awj::parse_arguments({L"--preset", L"fast"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.visual_quality.value_or(-1) != 25 ||
      parsed->config.encode_timeout_minutes !=
          awj::encoding_defaults::preset_fast_timeout_minutes) {
    return fail("preset defaults mismatch.");
  }

  parsed =
      awj::parse_arguments({L"--avif-encoder", L"aom", L"--chroma", L"444"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::aom ||
      parsed->config.chroma_mode != awj::ChromaMode::yuv444) {
    return fail("CLI AVIF encoder selection did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--avif-encoder", L"svt-av1-hdr", L"--svtav1hdr-crf", L"28",
       L"--svtav1hdr-preset", L"4", L"--svtav1hdr-tune", L"iq",
       L"--svtav1hdr-keyint", L"1", L"--color-primaries", L"9",
       L"--transfer-characteristics", L"16", L"--matrix-coefficients", L"9",
       L"--color-range", L"0"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::svt ||
      parsed->config.svtav1hdr_crf.value_or(-1) != 28 ||
      parsed->config.svtav1hdr_preset.value_or(-1) != 4 ||
      parsed->config.svtav1hdr_tune != "iq" ||
      parsed->config.svtav1hdr_keyint.value_or(-1) != 1 ||
      parsed->config.color_primaries.value_or(-1) != 9 ||
      parsed->config.transfer_characteristics.value_or(-1) != 16 ||
      parsed->config.matrix_coefficients.value_or(-1) != 9 ||
      parsed->config.color_range.value_or(-1) != 0) {
    return fail("CLI svt-av1-hdr options did not parse.");
  }

  parsed =
      awj::parse_arguments({L"--avif-encoder", L"svt", L"--chroma", L"444"});
  if (parsed || parsed.error().find("svt-av1-hdr 只支持 420 chroma") == std::string::npos) {
    return fail("CLI should reject explicit SVT 444 chroma.");
  }
  parsed =
      awj::parse_arguments({L"--avif-encoder", L"svt", L"--quality", L"100"});
  if (parsed || parsed.error().find("svt-av1-hdr 不支持 AVIF 无损") == std::string::npos) {
    return fail("CLI should reject explicit SVT q100.");
  }
  parsed =
      awj::parse_arguments({L"--avif-encoder", L"svt", L"--bit-depth", L"12"});
  if (parsed || parsed.error().find("svt-av1-hdr 只支持 8/10-bit") == std::string::npos) {
    return fail("CLI should reject explicit SVT 12-bit.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"zenrav1e"});
  if (!parsed ||
      parsed->config.avif_encoder != awj::AvifEncoderMode::zenrav1e ||
      !parsed->config.enable_experimental_encoders) {
    return fail(
        "CLI did not accept zenrav1e encoder with experimental encoders "
        "enabled by default.");
  }

  parsed = awj::parse_arguments(
      {L"--avif-encoder", L"zenrav1e", L"--no-experimental-encoders"});
  if (!parsed ||
      parsed->config.avif_encoder != awj::AvifEncoderMode::zenrav1e ||
      parsed->config.enable_experimental_encoders) {
    return fail("CLI did not parse experimental encoder disablement.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"rav1e"});
  if (parsed ||
      parsed.error().find("AVIF encoder 不支持") == std::string::npos) {
    return fail("CLI did not reject rav1e encoder.");
  }

  parsed = awj::parse_arguments({L"--no-wic-fallback"});
  if (!parsed || parsed->config.allow_wic_fallback) {
    return fail("CLI did not disable WIC fallback.");
  }

  parsed = awj::parse_arguments(
      {L"--backend", L"magick", L"--magick-path", L"tools"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("CLI still accepts removed external Magick adapter options.");
  }

  parsed = awj::parse_arguments({L"--max-resolution", L"1600"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("CLI still accepts removed max-resolution option.");
  }

  const auto help = awj::help_text();
  if (help.find(std::format("AVIF q{}",
                            awj::encoding_defaults::default_avif_quality)) ==
          std::string::npos ||
      help.find(std::format("WebP q{}",
                            awj::encoding_defaults::default_webp_quality)) ==
          std::string::npos ||
      help.find(std::format("JPGLI q{}",
                            awj::encoding_defaults::default_jpegli_quality)) ==
          std::string::npos ||
      help.find("jpgli|jpegli") == std::string::npos ||
      help.find("--jpegli-progressive-level") == std::string::npos ||
      help.find("--avif-encoder") == std::string::npos ||
      help.find("--experimental-encoders") == std::string::npos ||
      help.find("默认开启") == std::string::npos ||
      help.find("zenrav1e") == std::string::npos ||
      help.find("--svtav1hdr-crf") == std::string::npos ||
      help.find("--svtav1hdr-keyint") == std::string::npos ||
      help.find("--max-resolution") != std::string::npos) {
    return fail("help text does not reflect encoding defaults.");
  }

  return 0;
}
