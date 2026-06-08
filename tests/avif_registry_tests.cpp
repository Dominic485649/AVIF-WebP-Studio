#include <iostream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

import awj.avif_registry;
import awj.config;
import awj.encoding_defaults;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

bool contains_any(std::string_view text,
                  std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (text.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  auto selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::auto_keep,
          .requested_bit_depth = {},
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, false, false));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::svt ||
      selected->applied_chroma != awj::ChromaMode::yuv420 ||
      selected->applied_bit_depth != 10 ||
      selected->bit_depth_reason.find("10-bit") == std::string::npos) {
    return fail("auto small opaque request did not prefer available SVT 10-bit output.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::auto_keep,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, false, false));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::svt ||
      selected->applied_chroma != awj::ChromaMode::yuv420 ||
      selected->applied_bit_depth != 8) {
    return fail("auto small opaque 420/8-bit request did not select available SVT.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::auto_keep,
          .requested_bit_depth = 8,
          .has_alpha = true,
          .must_preserve_alpha = true,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->fallback_reason.find("alpha") == std::string::npos) {
    return fail("auto alpha request did not select AOM by default.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv444,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv444) {
    return fail("auto 444 request did not select AOM.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv422,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv422) {
    return fail("auto 422 request did not select AOM.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 12,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_bit_depth != 12) {
    return fail("auto 12-bit request did not select AOM.");
  }

  auto rejected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 14,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (rejected || rejected.error().find("14-bit") == std::string::npos) {
    return fail("auto >12-bit request was not rejected clearly.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 10,
          .visual_quality_search = true,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::svt) {
    return fail("auto visual-quality request should preserve normal capability-driven selection.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::auto_keep,
          .requested_bit_depth = 10,
          .pixel_count = awj::encoding_defaults::ordinary_large_safe_max_pixels + 1,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(true, true, true, true));
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->fallback_reason.find("SVT") == std::string::npos) {
    return fail("auto over-SVT-limit opaque 420-compatible request did not fall back to AOM.");
  }

  rejected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::auto_keep,
          .requested_bit_depth = 10,
          .has_alpha = true,
          .must_preserve_alpha = true,
          .pixel_count = awj::encoding_defaults::large_image_threshold_pixels + 1,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      awj::avif_encoder_capabilities_for_build(false, true, true, true));
  if (rejected || rejected.error().find("aom") == std::string::npos) {
    return fail("large alpha request should not incorrectly select zenrav1e without verified alpha policy.");
  }

  rejected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::svt,
      .requested_chroma = awj::ChromaMode::yuv444,
      .requested_bit_depth = 8,
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (rejected || !contains_any(rejected.error(), {"not available", "不可用"})) {
    return fail("explicit SVT request was not rejected clearly when unavailable.");
  }

  rejected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::zenrav1e,
      .requested_chroma = awj::ChromaMode::yuv444,
      .requested_bit_depth = 10,
      .pixel_count = 1024,
      .speed = awj::encoding_defaults::default_zenrav1e_preset});
  if (rejected || !contains_any(rejected.error(), {"not available", "不可用"})) {
    return fail("explicit zenrav1e request was not blocked clearly when bridge is unavailable.");
  }

  auto experimental_capabilities = awj::avif_encoder_capabilities_for_build(
      true, false, true, false);
  auto auto_experimental = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv444,
          .requested_bit_depth = 10,
          .pixel_count = 1024,
          .speed = awj::encoding_defaults::default_zenrav1e_preset},
      experimental_capabilities);
  if (!auto_experimental || auto_experimental->applied_encoder != awj::AvifEncoderMode::aom) {
    return fail("auto should not select zenrav1e even when the bridge is available.");
  }
  auto gated_zen = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::zenrav1e,
          .requested_chroma = awj::ChromaMode::yuv444,
          .requested_bit_depth = 10,
          .pixel_count = 1024,
          .speed = awj::encoding_defaults::default_zenrav1e_preset},
      experimental_capabilities);
  if (gated_zen || !contains_any(gated_zen.error(), {"experimental", "实验"})) {
    return fail("explicit zenrav1e request should require experimental encoders to be enabled.");
  }

  experimental_capabilities = awj::avif_encoder_capabilities_for_build(
      true, false, true, true);
  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::zenrav1e,
          .requested_chroma = awj::ChromaMode::yuv444,
          .requested_bit_depth = 10,
          .pixel_count = 1024,
          .speed = awj::encoding_defaults::default_zenrav1e_preset},
      experimental_capabilities);
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::zenrav1e ||
      selected->applied_chroma != awj::ChromaMode::yuv444 ||
      selected->applied_bit_depth != 10 || !selected->experimental) {
    return fail("explicit zenrav1e request was not selected as experimental encoder.");
  }
  auto diagnostics = awj::diagnostics_from_avif_selection(*selected);
  if (diagnostics.encoder_id != "zenrav1e" ||
      diagnostics.speed_mapping.codec_key != "zenravif:speed" ||
      !contains_any(diagnostics.bit_depth_reason, {"explicit", "明确请求"})) {
    return fail("zenrav1e diagnostics are incomplete.");
  }

  selected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::aom,
      .requested_chroma = awj::ChromaMode::yuv422,
      .requested_bit_depth = 10,
      .pixel_count = 1024,
      .speed = awj::encoding_defaults::default_aom_cpu_used});
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv422 ||
      selected->applied_bit_depth != 10) {
    return fail("explicit AOM 422/10-bit request did not stay on AOM.");
  }

  diagnostics = awj::diagnostics_from_avif_selection(*selected);
  if (diagnostics.encoder_id != "aom" || diagnostics.speed_mapping.codec_key != "aom:cpu-used" ||
      diagnostics.applied_chroma != "422" || diagnostics.applied_bit_depth != 10 ||
      !contains_any(diagnostics.bit_depth_reason, {"explicit", "明确请求"})) {
    return fail("AOM diagnostics are incomplete.");
  }

  auto unavailable_svt_capabilities = awj::avif_encoder_capabilities_for_build(true, false, false, false);
  rejected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::svt,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      unavailable_svt_capabilities);
  if (rejected || !contains_any(rejected.error(), {"not available", "不可用"})) {
    return fail("explicit SVT unavailable request was not rejected clearly.");
  }

  auto svt_capabilities = awj::avif_encoder_capabilities_for_build(true, true, false, false);
  auto auto_with_svt = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      svt_capabilities);
  if (!auto_with_svt || auto_with_svt->applied_encoder != awj::AvifEncoderMode::svt ||
      auto_with_svt->speed != awj::encoding_defaults::default_svtav1hdr_preset) {
    return fail("auto should prefer svt-av1-hdr for ordinary opaque 420-compatible requests and use its default preset.");
  }

  auto auto_disabled_svt_capabilities = awj::avif_encoder_capabilities_for_build(true, true, false, false);
  for (auto& capability : auto_disabled_svt_capabilities) {
    if (capability.mode == awj::AvifEncoderMode::svt) {
      capability.auto_selectable = false;
    }
  }
  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::automatic,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 8,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      auto_disabled_svt_capabilities);
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->fallback_reason.find("未参与 auto") == std::string::npos) {
    return fail("auto should skip SVT when the capability is not auto-selectable.");
  }

  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::svt,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 10,
          .pixel_count = 1024,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      svt_capabilities);
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::svt ||
      selected->applied_bit_depth != 10) {
    return fail("explicit svt-av1-hdr request was not selected when available.");
  }
  diagnostics = awj::diagnostics_from_avif_selection(*selected);
  if (diagnostics.encoder_id != "svt-av1-hdr" ||
      diagnostics.speed_mapping.codec_key != "svt-av1-hdr:preset") {
    return fail("svt-av1-hdr diagnostics are incomplete.");
  }
  for (auto& capability : svt_capabilities) {
    if (capability.mode == awj::AvifEncoderMode::svt) {
      capability.enabled = true;
    } else if (capability.mode == awj::AvifEncoderMode::aom) {
      capability.enabled = false;
    } else if (capability.mode == awj::AvifEncoderMode::zenrav1e) {
      capability.enabled = false;
    }
  }
  rejected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::svt,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = 10,
          .pixel_count = awj::encoding_defaults::svt_safe_max_pixels + 1,
          .speed = awj::default_speed_for(awj::OutputFormat::avif)},
      svt_capabilities);
  if (rejected || !contains_any(rejected.error(), {"over", "超过"})) {
    return fail("SVT over-pixel-limit rejection was not explicit.");
  }

  auto aom_8bit_capabilities = awj::avif_encoder_capabilities_for_build(true, false, false, false);
  for (auto& capability : aom_8bit_capabilities) {
    if (capability.mode == awj::AvifEncoderMode::aom) {
      capability.bit_depths = {8};
    }
  }
  selected = awj::select_avif_encoder_from_capabilities(
      awj::AvifEncoderSelectionRequest{
          .requested_encoder = awj::AvifEncoderMode::aom,
          .requested_chroma = awj::ChromaMode::yuv420,
          .requested_bit_depth = {},
          .pixel_count = 1024,
          .speed = awj::encoding_defaults::default_aom_cpu_used},
      aom_8bit_capabilities);
  if (!selected || selected->applied_bit_depth != 8 ||
      selected->bit_depth_reason.find("8-bit") == std::string::npos) {
    return fail("auto bit-depth did not fall back to 8-bit when 10-bit is unsupported.");
  }

  auto unsupported = awj::config_detail::parse_avif_encoder(L"rav1e");
  if (unsupported) {
    return fail("rav1e should not be accepted as an AVIF encoder mode.");
  }
  return 0;
}
