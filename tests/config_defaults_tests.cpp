#include <format>
#include <iostream>
#include <string>
#include <vector>

import awj.config;
import awj.encoding_defaults;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  const auto defaults = awj::default_app_config();
  if (defaults.input_path.native() != std::wstring{awj::encoding_defaults::default_input_path}) {
    return fail("default input path does not come from encoding defaults.");
  }
  if (defaults.output_template != awj::encoding_defaults::default_output_template) {
    return fail("default output template does not come from encoding defaults.");
  }
  if (defaults.quality != awj::encoding_defaults::default_avif_quality) {
    return fail("default AVIF quality mismatch.");
  }
  if (defaults.max_resolution != awj::encoding_defaults::default_max_resolution) {
    return fail("default max resolution mismatch.");
  }
  if (defaults.memory_limit_bytes != awj::encoding_defaults::default_memory_limit_bytes) {
    return fail("default memory limit mismatch.");
  }
  if (defaults.encode_timeout_minutes != awj::encoding_defaults::preset_best_timeout_minutes) {
    return fail("default encode timeout mismatch.");
  }

  if (awj::default_quality_for(awj::OutputFormat::avif) != awj::encoding_defaults::default_avif_quality ||
      awj::default_quality_for(awj::OutputFormat::webp) != awj::encoding_defaults::default_webp_quality ||
      awj::default_quality_for(awj::OutputFormat::jxl) != awj::encoding_defaults::default_jxl_quality) {
    return fail("format quality defaults mismatch.");
  }

  if (awj::default_speed_for(awj::OutputFormat::webp) != awj::encoding_defaults::default_native_speed ||
      awj::default_speed_for(awj::OutputFormat::avif) != awj::encoding_defaults::default_avif_native_speed ||
      awj::default_speed_for(awj::OutputFormat::jxl) != awj::encoding_defaults::default_jxl_native_speed) {
    return fail("format speed defaults mismatch.");
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

  parsed = awj::parse_arguments({L"--format", L"webp", L"--quality", L"82"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != 82) {
    return fail("explicit CLI quality was overwritten by defaults.");
  }

  parsed = awj::parse_arguments({L"--preset", L"fast"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != awj::encoding_defaults::preset_fast_quality ||
      parsed->config.encode_timeout_minutes != awj::encoding_defaults::preset_fast_timeout_minutes) {
    return fail("preset defaults mismatch.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"aom", L"--chroma", L"444"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::aom ||
      parsed->config.chroma_mode != awj::ChromaMode::yuv444) {
    return fail("CLI AVIF encoder selection did not parse.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"svt", L"--chroma", L"444"});
  if (parsed || parsed.error().find("only supports 420") == std::string::npos) {
    return fail("CLI did not reject SVT with 444 chroma.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"zenrav1e"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::zenrav1e ||
      parsed->config.enable_experimental_encoders) {
    return fail("CLI did not accept zenrav1e encoder while keeping experimental encoders disabled by default.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"zenrav1e", L"--experimental-encoders"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::zenrav1e ||
      !parsed->config.enable_experimental_encoders) {
    return fail("CLI did not parse experimental encoder enablement.");
  }

  parsed = awj::parse_arguments({L"--avif-encoder", L"rav1e"});
  if (parsed || parsed.error().find("AVIF encoder 不支持") == std::string::npos) {
    return fail("CLI did not reject rav1e encoder.");
  }

  const auto help = awj::help_text();
  if (help.find(std::format("AVIF 默认 {}", awj::encoding_defaults::default_avif_quality)) == std::string::npos ||
      help.find(std::format("WebP/JXL 默认 {}", awj::encoding_defaults::default_webp_quality)) == std::string::npos ||
      help.find("--avif-encoder") == std::string::npos ||
      help.find("--experimental-encoders") == std::string::npos) {
    return fail("help text does not reflect encoding defaults.");
  }

  return 0;
}
