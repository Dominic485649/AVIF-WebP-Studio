module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.avif_registry;

import awj.codec;
import awj.config;
import awj.encoding_defaults;

export namespace awj {

struct AvifEncoderCapability {
  AvifEncoderMode mode{AvifEncoderMode::automatic};
  std::string id{};
  std::vector<ChromaMode> chroma_modes{};
  std::vector<int> bit_depths{};
  bool supports_alpha{};
  bool supports_avif_grid{};
  std::optional<std::uint32_t> max_single_image_width{};
  std::optional<std::uint32_t> max_single_image_height{};
  bool experimental{};
  bool enabled{true};
  bool feature_enabled{true};
  bool auto_selectable{true};
  bool auto_alpha_selectable{};
  std::string unavailable_reason{};
  std::string license{};
  int default_speed{};
};

struct AvifEncoderSelectionRequest {
  AvifEncoderMode requested_encoder{AvifEncoderMode::automatic};
  ChromaMode requested_chroma{ChromaMode::auto_keep};
  std::optional<int> requested_bit_depth{};
  std::string requested_bit_depth_reason{};
  bool has_alpha{};
  bool must_preserve_alpha{};
  bool visual_quality_search{};
  bool speed_explicit{};
  bool allow_zenrav1e_alpha{};
  std::uint64_t pixel_count{};
  std::uint32_t width{};
  std::uint32_t height{};
  int speed{default_speed_for(OutputFormat::avif)};
};

struct AvifEncoderSelection {
  AvifEncoderMode requested_encoder{AvifEncoderMode::automatic};
  AvifEncoderMode applied_encoder{AvifEncoderMode::automatic};
  ChromaMode requested_chroma{ChromaMode::auto_keep};
  ChromaMode applied_chroma{ChromaMode::auto_keep};
  std::optional<int> requested_bit_depth{};
  std::optional<int> applied_bit_depth{};
  std::string bit_depth_reason{};
  std::uint64_t pixel_count{};
  int speed{};
  bool experimental{};
  std::string license{};
  std::string fallback_reason{};
};

namespace avif_registry_detail {

bool contains_chroma(const AvifEncoderCapability& capability, ChromaMode chroma) {
  return std::ranges::find(capability.chroma_modes, chroma) != capability.chroma_modes.end();
}

bool contains_bit_depth(const AvifEncoderCapability& capability, int bit_depth) {
  return std::ranges::find(capability.bit_depths, bit_depth) != capability.bit_depths.end();
}

ChromaMode applied_chroma_for(const AvifEncoderCapability& capability,
                              ChromaMode requested) {
  if (requested == ChromaMode::auto_keep) {
    return contains_chroma(capability, ChromaMode::yuv420) ? ChromaMode::yuv420
                                                          : capability.chroma_modes.front();
  }
  return requested;
}

struct AppliedBitDepth {
  std::optional<int> value{};
  std::string reason{};
};

AppliedBitDepth applied_bit_depth_for(const AvifEncoderCapability& capability,
                                      const AvifEncoderSelectionRequest& request) {
  if (request.requested_bit_depth) {
    return AppliedBitDepth{.value = *request.requested_bit_depth,
                           .reason = request.requested_bit_depth_reason.empty()
                                         ? "explicit bit-depth requested"
                                         : request.requested_bit_depth_reason};
  }
  if (contains_bit_depth(capability, 10)) {
    return AppliedBitDepth{.value = 10,
                           .reason = "auto selected preferred 10-bit output"};
  }
  if (contains_bit_depth(capability, 8)) {
    return AppliedBitDepth{.value = 8,
                           .reason = "auto fell back to 8-bit because 10-bit is unsupported"};
  }
  return AppliedBitDepth{.value = capability.bit_depths.empty()
                                      ? std::optional<int>{}
                                      : std::optional<int>{capability.bit_depths.front()},
                         .reason = "auto selected first supported bit-depth"};
}

bool capability_enabled_for_selection(const AvifEncoderCapability& capability) {
  return capability.enabled && (!capability.experimental || capability.feature_enabled);
}

bool capability_enabled_for_auto(const AvifEncoderCapability& capability) {
  return capability_enabled_for_selection(capability) && capability.auto_selectable;
}

bool capability_matches(const AvifEncoderCapability& capability,
                        const AvifEncoderSelectionRequest& request) {
  if (!capability_enabled_for_selection(capability)) {
    return false;
  }
  if (request.must_preserve_alpha && !capability.supports_alpha) {
    return false;
  }
  const auto chroma = applied_chroma_for(capability, request.requested_chroma);
  if (!contains_chroma(capability, chroma)) {
    return false;
  }
  const auto bit_depth = applied_bit_depth_for(capability, request);
  if (bit_depth.value && !contains_bit_depth(capability, *bit_depth.value)) {
    return false;
  }
  if (capability.max_single_image_width && request.width > *capability.max_single_image_width) {
    return false;
  }
  if (capability.max_single_image_height && request.height > *capability.max_single_image_height) {
    return false;
  }
  if (capability.mode == AvifEncoderMode::svt &&
      request.pixel_count > encoding_defaults::svt_safe_max_pixels) {
    return false;
  }
  return true;
}

std::string explicit_rejection_reason(const AvifEncoderCapability& capability,
                                      const AvifEncoderSelectionRequest& request) {
  if (!capability.enabled) {
    if (!capability.unavailable_reason.empty()) {
      return capability.unavailable_reason;
    }
    return std::format("AVIF encoder {} is not available in this build.",
                       capability.id);
  }
  if (capability.experimental && !capability.feature_enabled) {
    return std::format("AVIF encoder {} is experimental and disabled in this build; enable the feature flag and review its license before use.",
                       capability.id);
  }
  if (request.must_preserve_alpha && !capability.supports_alpha) {
    if (capability.mode == AvifEncoderMode::svt) {
      return "svt-av1-hdr AVIF encoder does not support preserving alpha; use --alpha auto/off or --avif-encoder auto/aom.";
    }
    return std::format("AVIF encoder {} does not support preserving alpha.", capability.id);
  }
  const auto chroma = applied_chroma_for(capability, request.requested_chroma);
  if (!contains_chroma(capability, chroma)) {
    if (capability.mode == AvifEncoderMode::svt) {
      return "svt-av1-hdr AVIF encoder only supports 420 chroma; use --chroma 420/auto or --avif-encoder aom.";
    }
    return std::format("AVIF encoder {} does not support requested chroma {}.",
                       capability.id, chroma_mode_name(request.requested_chroma));
  }
  const auto bit_depth = applied_bit_depth_for(capability, request);
  if (bit_depth.value && !contains_bit_depth(capability, *bit_depth.value)) {
    return std::format("AVIF encoder {} does not support requested {}-bit output.",
                       capability.id, *bit_depth.value);
  }
  if (capability.max_single_image_width && request.width > *capability.max_single_image_width) {
    if (capability.mode == AvifEncoderMode::zenrav1e) {
      return std::format("zenrav1e 是单图 AVIF 编码，输入边长超过 {}。",
                         *capability.max_single_image_width);
    }
    return std::format("AVIF encoder {} does not support input width {} over {}.",
                       capability.id, request.width, *capability.max_single_image_width);
  }
  if (capability.max_single_image_height && request.height > *capability.max_single_image_height) {
    if (capability.mode == AvifEncoderMode::zenrav1e) {
      return std::format("zenrav1e 是单图 AVIF 编码，输入边长超过 {}。",
                         *capability.max_single_image_height);
    }
    return std::format("AVIF encoder {} does not support input height {} over {}.",
                       capability.id, request.height, *capability.max_single_image_height);
  }
  if (capability.mode == AvifEncoderMode::svt &&
      request.pixel_count > encoding_defaults::svt_safe_max_pixels) {
    return std::format("svt-av1-hdr AVIF encoder is disabled for images over {} pixels; use --avif-encoder auto/aom.",
                       encoding_defaults::svt_safe_max_pixels);
  }
  return std::format("AVIF encoder {} is not available for this request.", capability.id);
}

AvifEncoderSelection make_selection(const AvifEncoderCapability& capability,
                                    const AvifEncoderSelectionRequest& request,
                                    std::string fallback_reason) {
  const auto bit_depth = applied_bit_depth_for(capability, request);
  return AvifEncoderSelection{
      .requested_encoder = request.requested_encoder,
      .applied_encoder = capability.mode,
      .requested_chroma = request.requested_chroma,
      .applied_chroma = applied_chroma_for(capability, request.requested_chroma),
      .requested_bit_depth = request.requested_bit_depth,
      .applied_bit_depth = bit_depth.value,
      .bit_depth_reason = std::move(bit_depth.reason),
      .pixel_count = request.pixel_count,
      .speed = request.speed_explicit ? request.speed : capability.default_speed,
      .experimental = capability.experimental,
      .license = capability.license,
      .fallback_reason = std::move(fallback_reason)};
}

const AvifEncoderCapability* find_capability(std::span<const AvifEncoderCapability> capabilities,
                                             AvifEncoderMode mode) {
  const auto it = std::ranges::find(capabilities, mode, &AvifEncoderCapability::mode);
  return it == capabilities.end() ? nullptr : &*it;
}

bool is_explicit_non_420(ChromaMode chroma) noexcept {
  return chroma == ChromaMode::yuv422 || chroma == ChromaMode::yuv444;
}

bool is_420_compatible(ChromaMode chroma) noexcept {
  return chroma == ChromaMode::auto_keep || chroma == ChromaMode::yuv420;
}

bool requested_bit_depth_above_10(const AvifEncoderSelectionRequest& request) noexcept {
  return request.requested_bit_depth && *request.requested_bit_depth > 10;
}

std::expected<AvifEncoderSelection, std::string> select_auto_capability(
    const AvifEncoderSelectionRequest& request,
    std::span<const AvifEncoderCapability> capabilities) {
  if (request.requested_bit_depth && *request.requested_bit_depth > 12) {
    return std::unexpected{std::format("AVIF auto does not support requested {}-bit output.",
                                       *request.requested_bit_depth)};
  }

  const auto select = [&](AvifEncoderMode mode,
                          std::string fallback_reason) -> std::expected<AvifEncoderSelection, std::string> {
    const auto* capability = find_capability(capabilities, mode);
    if (capability == nullptr) {
      return std::unexpected{"Requested AVIF encoder is not registered."};
    }
    if (!capability_matches(*capability, request)) {
      return std::unexpected{explicit_rejection_reason(*capability, request)};
    }
    return make_selection(*capability, request, std::move(fallback_reason));
  };

  if (request.must_preserve_alpha) {
    const auto* zenrav1e = find_capability(capabilities, AvifEncoderMode::zenrav1e);
    if (request.allow_zenrav1e_alpha && zenrav1e != nullptr && zenrav1e->auto_alpha_selectable &&
        capability_matches(*zenrav1e, request)) {
      return make_selection(*zenrav1e, request,
                            "auto selected zenrav1e for alpha after verified round-trip support.");
    }
    return select(AvifEncoderMode::aom, "auto fell back to AOM because alpha must be preserved.");
  }

  if (is_explicit_non_420(request.requested_chroma)) {
    return select(AvifEncoderMode::aom, "auto fell back to AOM because chroma was explicitly requested above 420.");
  }

  if (requested_bit_depth_above_10(request)) {
    return select(AvifEncoderMode::aom, "auto fell back to AOM because bit-depth above 10 was explicitly requested.");
  }

  auto svt = select(AvifEncoderMode::svt, {});
  if (svt) {
    return svt;
  }
  return select(AvifEncoderMode::aom, std::format("auto fell back to AOM because SVT was unavailable: {}", svt.error()));
}

}  // namespace awj_registry_detail

export std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_experimental(
    bool enable_experimental) {
  return {
      AvifEncoderCapability{.mode = AvifEncoderMode::svt,
                            .id = "svt-av1-hdr",
                            .chroma_modes = {ChromaMode::yuv420},
                            .bit_depths = {8, 10},
                            .supports_alpha = false,
                            .supports_avif_grid = false,
                            .enabled = false,
                            .auto_selectable = false,
                            .unavailable_reason = "AVIF encoder svt-av1-hdr is not available in this build; the static libavif/SVT backend was not built.",
                            .license = "BSD-3-Clause",
                            .default_speed = encoding_defaults::default_svtav1hdr_preset},
      AvifEncoderCapability{.mode = AvifEncoderMode::aom,
                            .id = "aom",
                            .chroma_modes = {ChromaMode::yuv420, ChromaMode::yuv422,
                                             ChromaMode::yuv444},
                            .bit_depths = {8, 10, 12},
                            .supports_alpha = true,
                            .supports_avif_grid = true,
                            .license = "BSD-2-Clause",
                            .default_speed = encoding_defaults::default_aom_cpu_used},
      AvifEncoderCapability{.mode = AvifEncoderMode::zenrav1e,
                            .id = "zenrav1e",
                            .chroma_modes = {ChromaMode::yuv420, ChromaMode::yuv444},
                            .bit_depths = {8, 10, 12},
                            .supports_alpha = true,
                            .supports_avif_grid = false,
                            .max_single_image_width = encoding_defaults::avif_single_image_max_dimension,
                            .max_single_image_height = encoding_defaults::avif_single_image_max_dimension,
                            .experimental = true,
                            .enabled = false,
                            .feature_enabled = enable_experimental,
                            .auto_selectable = false,
                            .unavailable_reason = "AVIF encoder zenrav1e is not available in this build; the zenravif bridge was not built.",
                            .license = "AGPL-3.0-only OR LicenseRef-Imazen-Commercial",
                            .default_speed = encoding_defaults::default_zenrav1e_preset},
  };
}

export std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_build(
    bool aom_available,
    bool svt_available,
    bool zenravif_available,
    bool enable_experimental) {
  auto capabilities = avif_encoder_capabilities_for_experimental(enable_experimental);
  for (auto& capability : capabilities) {
    switch (capability.mode) {
      case AvifEncoderMode::aom:
        capability.enabled = aom_available;
        if (!aom_available) {
          capability.unavailable_reason = "AVIF encoder aom is not available in this libavif build.";
        }
        break;
      case AvifEncoderMode::svt:
        capability.enabled = svt_available;
        capability.auto_selectable = svt_available;
        if (svt_available) {
          capability.unavailable_reason.clear();
        }
        break;
      case AvifEncoderMode::zenrav1e:
        capability.enabled = zenravif_available;
        capability.feature_enabled = enable_experimental;
        capability.auto_selectable = false;
        if (zenravif_available) {
          capability.unavailable_reason.clear();
        }
        break;
      case AvifEncoderMode::automatic:
      default:
        break;
    }
  }
  return capabilities;
}

export std::vector<AvifEncoderCapability> avif_encoder_capabilities() {
  return avif_encoder_capabilities_for_experimental(false);
}

export std::expected<AvifEncoderSelection, std::string> select_avif_encoder_from_capabilities(
    const AvifEncoderSelectionRequest& request,
    std::span<const AvifEncoderCapability> capabilities) {
  if (request.requested_bit_depth && *request.requested_bit_depth > 12) {
    return std::unexpected{std::format("AVIF encoder does not support requested {}-bit output.",
                                       *request.requested_bit_depth)};
  }
  if (request.requested_encoder != AvifEncoderMode::automatic) {
    const auto it = std::ranges::find(capabilities, request.requested_encoder,
                                      &AvifEncoderCapability::mode);
    if (it == capabilities.end()) {
      return std::unexpected{"Requested AVIF encoder is not registered."};
    }
    if (!avif_registry_detail::capability_matches(*it, request)) {
      return std::unexpected{avif_registry_detail::explicit_rejection_reason(*it, request)};
    }
    return avif_registry_detail::make_selection(*it, request, {});
  }

  return avif_registry_detail::select_auto_capability(request, capabilities);
}

export std::expected<AvifEncoderSelection, std::string> select_avif_encoder(
    const AvifEncoderSelectionRequest& request) {
  const auto capabilities = avif_encoder_capabilities();
  return select_avif_encoder_from_capabilities(request, capabilities);
}

SpeedMapping avif_speed_mapping_for(const AvifEncoderSelection& selection) {
  switch (selection.applied_encoder) {
    case AvifEncoderMode::aom:
      return SpeedMapping{.user_speed = selection.speed,
                          .codec_value = selection.speed,
                          .codec_key = "aom:cpu-used"};
    case AvifEncoderMode::zenrav1e:
      return SpeedMapping{.user_speed = selection.speed,
                          .codec_value = selection.speed,
                          .codec_key = "zenravif:speed"};
    case AvifEncoderMode::svt:
      return SpeedMapping{.user_speed = selection.speed,
                          .codec_value = encoding_defaults::default_svtav1hdr_preset,
                          .codec_key = "svt-av1-hdr:preset"};
    case AvifEncoderMode::automatic:
    default:
      return SpeedMapping{.user_speed = selection.speed,
                          .codec_value = selection.speed,
                          .codec_key = "aom:cpu-used"};
  }
}

EncodeDiagnostics diagnostics_from_avif_selection(
    const AvifEncoderSelection& selection) {
  return EncodeDiagnostics{.encoder_id = avif_encoder_mode_name(selection.applied_encoder),
                           .requested_encoder_id = avif_encoder_mode_name(selection.requested_encoder),
                           .requested_chroma = chroma_mode_name(selection.requested_chroma),
                           .applied_chroma = chroma_mode_name(selection.applied_chroma),
                           .requested_bit_depth = selection.requested_bit_depth,
                           .applied_bit_depth = selection.applied_bit_depth,
                           .bit_depth_reason = selection.bit_depth_reason,
                           .fallback_reason = selection.fallback_reason,
                           .encoder_experimental = selection.experimental,
                           .encoder_license = selection.license,
                           .speed_mapping = avif_speed_mapping_for(selection)};
}

}  // namespace awj
