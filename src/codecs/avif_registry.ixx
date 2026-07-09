module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.avif_registry;

import awj.codec;
import awj.config;
import awj.encoding_defaults;

export namespace awj {

namespace avif_registry_detail {

bool contains_chroma(const AvifEncoderCapability& capability, ChromaMode chroma) {
  return std::ranges::find(capability.chroma_modes, chroma) != capability.chroma_modes.end();
}

bool contains_bit_depth(const AvifEncoderCapability& capability, int bit_depth) {
  return std::ranges::find(capability.bit_depths, bit_depth) != capability.bit_depths.end();
}

std::optional<int> max_bit_depth_for(const AvifEncoderCapability& capability) {
  if (capability.bit_depths.empty()) {
    return {};
  }
  return *std::ranges::max_element(capability.bit_depths);
}

std::optional<int> nearest_supported_bit_depth_not_exceeding(
    const AvifEncoderCapability& capability,
    int requested) {
  std::optional<int> best{};
  for (const int bit_depth : capability.bit_depths) {
    if (bit_depth <= requested && (!best || bit_depth > *best)) {
      best = bit_depth;
    }
  }
  return best;
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
    const int requested = *request.requested_bit_depth;
    const auto requested_reason =
        request.requested_bit_depth_reason.empty()
            ? (request.requested_bit_depth_explicit ? std::string{"用户明确请求 bit-depth"}
                                                    : std::format("源图继承 {}-bit bit-depth", requested))
            : request.requested_bit_depth_reason;
    if (contains_bit_depth(capability, requested) || request.requested_bit_depth_explicit) {
      return AppliedBitDepth{.value = requested, .reason = requested_reason};
    }
    const auto clamped = nearest_supported_bit_depth_not_exceeding(capability, requested);
    if (clamped) {
      const auto max_supported = max_bit_depth_for(capability);
      const auto clamp_reason =
          max_supported && requested > *max_supported
              ? std::format("源图 {}-bit 超过 {} 支持上限，限制为 {}-bit 输出",
                            requested, capability.id, *clamped)
              : std::format("源图 {}-bit 不受 {} 支持，限制为 {}-bit 输出",
                            requested, capability.id, *clamped);
      return AppliedBitDepth{.value = *clamped,
                             .reason = requested_reason.empty()
                                           ? clamp_reason
                                           : std::format("{}；{}", requested_reason, clamp_reason)};
    }
    return AppliedBitDepth{.value = requested, .reason = requested_reason};
  }
  if (contains_bit_depth(capability, 10)) {
    return AppliedBitDepth{.value = 10,
                           .reason = "auto 选择首选 10-bit 输出"};
  }
  if (contains_bit_depth(capability, 8)) {
    return AppliedBitDepth{.value = 8,
                           .reason = "auto 回退到 8-bit，因为当前编码器不支持 10-bit"};
  }
  return AppliedBitDepth{.value = capability.bit_depths.empty()
                                      ? std::optional<int>{}
                                      : std::optional<int>{capability.bit_depths.front()},
                         .reason = "auto 选择第一个受支持 bit-depth"};
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
  if (capability.mode == AvifEncoderMode::aom &&
      request.pixel_count > encoding_defaults::avif_single_image_max_pixels) {
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
    return std::format("AVIF encoder {} 在当前构建中不可用。",
                       capability.id);
  }
  if (capability.experimental && !capability.feature_enabled) {
    return std::format("AVIF encoder {} 是实验性编码器，当前构建未启用；请启用对应 feature flag 并确认许可证后再使用。",
                       capability.id);
  }
  if (request.must_preserve_alpha && !capability.supports_alpha) {
    if (capability.mode == AvifEncoderMode::svt) {
      return "svt-av1-hdr AVIF encoder 不支持保留 alpha；请使用 --alpha auto/off 或 --avif-encoder auto/aom。";
    }
    return std::format("AVIF encoder {} 不支持保留 alpha。", capability.id);
  }
  const auto chroma = applied_chroma_for(capability, request.requested_chroma);
  if (!contains_chroma(capability, chroma)) {
    if (capability.mode == AvifEncoderMode::svt) {
      return "svt-av1-hdr AVIF encoder 只支持 420 chroma；请使用 --chroma 420/auto 或 --avif-encoder aom。";
    }
    return std::format("AVIF encoder {} 不支持请求的 chroma {}。",
                       capability.id, chroma_mode_name(request.requested_chroma));
  }
  const auto bit_depth = applied_bit_depth_for(capability, request);
  if (bit_depth.value && !contains_bit_depth(capability, *bit_depth.value)) {
    return std::format("AVIF encoder {} 不支持请求的 {}-bit 输出。",
                       capability.id, *bit_depth.value);
  }
  if (capability.max_single_image_width && request.width > *capability.max_single_image_width) {
    if (capability.mode == AvifEncoderMode::zenrav1e) {
      return std::format("zenrav1e 是单图 AVIF 编码，输入边长超过 {}。",
                         *capability.max_single_image_width);
    }
    return std::format("AVIF encoder {} 不支持输入宽度 {} 超过 {}。",
                       capability.id, request.width, *capability.max_single_image_width);
  }
  if (capability.max_single_image_height && request.height > *capability.max_single_image_height) {
    if (capability.mode == AvifEncoderMode::zenrav1e) {
      return std::format("zenrav1e 是单图 AVIF 编码，输入边长超过 {}。",
                         *capability.max_single_image_height);
    }
    return std::format("AVIF encoder {} 不支持输入高度 {} 超过 {}。",
                       capability.id, request.height, *capability.max_single_image_height);
  }
  if (capability.mode == AvifEncoderMode::aom &&
      request.pixel_count > encoding_defaults::avif_single_image_max_pixels) {
    return std::format("AOM/libaom AVIF 单图上限为 65536 边 / {} 像素；将自动走 zenrav1e/grid 大图链路。",
                       encoding_defaults::avif_single_image_max_pixels);
  }
  if (capability.mode == AvifEncoderMode::svt &&
      request.pixel_count > encoding_defaults::svt_safe_max_pixels) {
    return std::format("svt-av1-hdr AVIF encoder 单图上限为 {}x{} / {} 像素；请使用 --avif-encoder auto/aom。",
                       encoding_defaults::svtav1hdr_single_image_max_width,
                       encoding_defaults::svtav1hdr_single_image_max_height,
                       encoding_defaults::svt_safe_max_pixels);
  }
  return std::format("AVIF encoder {} 不适用于当前请求。", capability.id);
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
  if (request.requested_bit_depth && request.requested_bit_depth_explicit &&
      *request.requested_bit_depth > 12) {
    return std::unexpected{std::format("AVIF auto 不支持请求的 {}-bit 输出。",
                                       *request.requested_bit_depth)};
  }

  const auto select = [&](AvifEncoderMode mode,
                          std::string fallback_reason) -> std::expected<AvifEncoderSelection, std::string> {
    const auto* capability = find_capability(capabilities, mode);
    if (capability == nullptr) {
      return std::unexpected{"请求的 AVIF encoder 未注册。"};
    }
    if (!capability_enabled_for_auto(*capability)) {
      if (!capability_enabled_for_selection(*capability)) {
        return std::unexpected{explicit_rejection_reason(*capability, request)};
      }
      return std::unexpected{std::format("AVIF encoder {} 当前未参与 auto 选择。", capability->id)};
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
                            "auto 选择 zenrav1e，因为 alpha 已通过 round-trip 支持验证。");
    }
    return select(AvifEncoderMode::aom, "auto 回退到 AOM，因为必须保留 alpha。");
  }

  if (is_explicit_non_420(request.requested_chroma)) {
    return select(AvifEncoderMode::aom, "auto 回退到 AOM，因为用户明确请求高于 420 的 chroma。");
  }

  if (requested_bit_depth_above_10(request)) {
    if (request.requested_bit_depth_explicit) {
      return select(AvifEncoderMode::aom,
                    "auto 回退到 AOM，因为用户明确请求高于 10-bit 的 bit-depth。");
    }
    auto aom = select(AvifEncoderMode::aom,
                      "auto 回退到 AOM，因为源图 bit-depth 高于 SVT 支持上限。");
    if (aom) {
      return aom;
    }
    return select(AvifEncoderMode::svt,
                  std::format("auto 回退到 SVT-AV1-HDR 并限制源图 bit-depth，因为 AOM 不可用: {}",
                              aom.error()));
  }

  auto aom = select(AvifEncoderMode::aom, {});
  if (aom) {
    return aom;
  }
  return select(AvifEncoderMode::svt, std::format("auto 回退到 SVT-AV1-HDR，因为 AOM 不可用: {}", aom.error()));
}

}  // namespace awj_registry_detail

std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_experimental(
    bool enable_experimental) {
  return {
      AvifEncoderCapability{.mode = AvifEncoderMode::svt,
                            .id = "svt-av1-hdr",
                            .chroma_modes = {ChromaMode::yuv420},
                            .bit_depths = {8, 10},
                            .supports_alpha = false,
                            .supports_avif_grid = false,
                            .max_single_image_width = encoding_defaults::svtav1hdr_single_image_max_width,
                            .max_single_image_height = encoding_defaults::svtav1hdr_single_image_max_height,
                            .enabled = false,
                            .auto_selectable = false,
                            .unavailable_reason = "AVIF encoder svt-av1-hdr 在当前构建中不可用；未构建静态 libavif/SVT 后端。",
                            .license = "BSD-3-Clause",
                            .default_speed = encoding_defaults::default_svtav1hdr_preset},
      AvifEncoderCapability{.mode = AvifEncoderMode::aom,
                            .id = "aom",
                            .chroma_modes = {ChromaMode::yuv420, ChromaMode::yuv422,
                                             ChromaMode::yuv444},
                            .bit_depths = {8, 10, 12},
                            .supports_alpha = true,
                            .supports_avif_grid = true,
                            .max_single_image_width = encoding_defaults::avif_single_image_max_dimension,
                            .max_single_image_height = encoding_defaults::avif_single_image_max_dimension,
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
                            .unavailable_reason = "AVIF encoder zenrav1e 在当前构建中不可用；未构建 zenravif bridge。",
                            .license = "AGPL-3.0-only OR LicenseRef-Imazen-Commercial",
                            .default_speed = encoding_defaults::default_zenrav1e_preset},
  };
}

std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_build(
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
          capability.unavailable_reason = "AVIF encoder aom 在当前 libavif 构建中不可用。";
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

std::vector<AvifEncoderCapability> avif_encoder_capabilities() {
  return avif_encoder_capabilities_for_experimental(false);
}

std::expected<AvifEncoderSelection, std::string> select_avif_encoder_from_capabilities(
    const AvifEncoderSelectionRequest& request,
    std::span<const AvifEncoderCapability> capabilities) {
  if (request.requested_bit_depth && request.requested_bit_depth_explicit &&
      *request.requested_bit_depth > 12) {
    return std::unexpected{std::format("AVIF encoder 不支持请求的 {}-bit 输出。",
                                       *request.requested_bit_depth)};
  }
  if (request.requested_encoder != AvifEncoderMode::automatic) {
    const auto it = std::ranges::find(capabilities, request.requested_encoder,
                                      &AvifEncoderCapability::mode);
    if (it == capabilities.end()) {
      return std::unexpected{"请求的 AVIF encoder 未注册。"};
    }
    if (!avif_registry_detail::capability_matches(*it, request)) {
      return std::unexpected{avif_registry_detail::explicit_rejection_reason(*it, request)};
    }
    return avif_registry_detail::make_selection(*it, request, {});
  }

  return avif_registry_detail::select_auto_capability(request, capabilities);
}

std::expected<AvifEncoderSelection, std::string> select_avif_encoder(
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
