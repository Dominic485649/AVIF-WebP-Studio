#include <iostream>
#include <optional>
#include <string>

import awj.avif_registry;
import awj.config;
import awj.encoding_defaults;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  auto selected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::automatic,
      .requested_chroma = awj::ChromaMode::auto_keep,
      .requested_bit_depth = {},
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv420 ||
      selected->applied_bit_depth != 10 ||
      selected->bit_depth_reason.find("10-bit") == std::string::npos) {
    return fail("auto small request did not prefer available AOM 10-bit output.");
  }

  selected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::automatic,
      .requested_chroma = awj::ChromaMode::auto_keep,
      .requested_bit_depth = 8,
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv420 ||
      selected->applied_bit_depth != 8) {
    return fail("auto small 420/8-bit request did not select available AOM.");
  }

  selected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::automatic,
      .requested_chroma = awj::ChromaMode::yuv444,
      .requested_bit_depth = 8,
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_chroma != awj::ChromaMode::yuv444) {
    return fail("auto 444 request did not select AOM.");
  }

  selected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::automatic,
      .requested_chroma = awj::ChromaMode::yuv420,
      .requested_bit_depth = 12,
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (!selected || selected->applied_encoder != awj::AvifEncoderMode::aom ||
      selected->applied_bit_depth != 12) {
    return fail("auto 12-bit request did not select AOM.");
  }

  auto rejected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::svt,
      .requested_chroma = awj::ChromaMode::yuv444,
      .requested_bit_depth = 8,
      .pixel_count = 1024,
      .speed = awj::default_speed_for(awj::OutputFormat::avif)});
  if (rejected || rejected.error().find("not available") == std::string::npos) {
    return fail("explicit SVT request was not rejected clearly when unavailable.");
  }

  rejected = awj::select_avif_encoder(awj::AvifEncoderSelectionRequest{
      .requested_encoder = awj::AvifEncoderMode::zenrav1e,
      .requested_chroma = awj::ChromaMode::yuv444,
      .requested_bit_depth = 10,
      .pixel_count = 1024,
      .speed = awj::encoding_defaults::default_zenrav1e_preset});
  if (rejected || rejected.error().find("not available") == std::string::npos) {
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
  if (gated_zen || gated_zen.error().find("experimental") == std::string::npos) {
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
      diagnostics.bit_depth_reason.find("explicit") == std::string::npos) {
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
      diagnostics.bit_depth_reason.find("explicit") == std::string::npos) {
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
  if (rejected || rejected.error().find("not available") == std::string::npos) {
    return fail("explicit SVT unavailable request was not rejected clearly.");
  }

  auto svt_capabilities = awj::avif_encoder_capabilities_for_build(true, true, false, false);
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
  if (rejected || rejected.error().find("over") == std::string::npos) {
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
