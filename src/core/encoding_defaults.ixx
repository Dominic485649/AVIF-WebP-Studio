module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

export module awj.encoding_defaults;

export namespace awj::encoding_defaults {

inline constexpr std::string_view default_input_path_text = "input";
inline constexpr std::wstring_view default_input_path = L"input";
inline constexpr std::string_view default_output_template_text = "{name}";
inline constexpr std::wstring_view default_output_template = L"{name}";
inline constexpr std::string_view default_memory_limit_text = "auto";

inline constexpr int default_avif_quality = 70;
inline constexpr int default_webp_quality = 95;
inline constexpr int default_jxl_quality = 85;
inline constexpr int default_jpegli_quality = 90;
inline constexpr int default_webp_bit_depth = 8;
inline constexpr int default_quality = default_avif_quality;
inline constexpr int default_visual_quality = 90;

inline constexpr int preset_fast_quality = 75;
inline constexpr int preset_balanced_quality = 85;
inline constexpr int preset_best_quality = default_avif_quality;
inline constexpr int preset_extreme_quality = 95;

inline constexpr int preset_fast_timeout_minutes = 10;
inline constexpr int preset_balanced_timeout_minutes = 20;
inline constexpr int preset_best_timeout_minutes = 30;
inline constexpr int preset_extreme_timeout_minutes = 60;

inline constexpr std::uint64_t default_memory_limit_bytes = 0;
inline constexpr bool default_allow_wic_fallback = true;

inline constexpr int default_aom_cpu_used = 6;
inline constexpr int default_native_speed = 5;
inline constexpr int default_avif_native_speed = default_aom_cpu_used;
inline constexpr int default_webp_native_speed = 4;
inline constexpr int default_jxl_native_speed = 6;
inline constexpr int default_jpegli_native_speed = 5;
inline constexpr int default_jxl_effort = 4;
inline constexpr bool default_jxl_effort_is_explicit = false;
inline constexpr int default_jpegli_progressive_level = 2;
inline constexpr bool default_jpegli_optimize_huffman = true;
inline constexpr bool default_jpegli_xyb = false;
inline constexpr int default_zenrav1e_preset = 6;
inline constexpr int default_svt_preset = 6;
inline constexpr int default_svtav1hdr_crf = 30;
inline constexpr int default_svtav1hdr_preset = 6;
inline constexpr std::string_view default_svtav1hdr_tune = "3";
inline constexpr int default_svtav1hdr_keyint = 1;
inline constexpr bool default_svtav1hdr_avif = true;

inline constexpr std::uint64_t max_input_file_bytes =
    20ull * 1024ull * 1024ull * 1024ull;
// ponytail: session unlock for huge inputs; default stays 20 GiB. Not persisted.
inline std::atomic_bool unlock_max_input_file_bytes{false};
inline std::uint64_t effective_max_input_file_bytes() noexcept {
  // Unlocked path still caps absurd values to avoid size_t wrap in buffers.
  constexpr std::uint64_t unlocked_cap = 512ull * 1024ull * 1024ull * 1024ull; // 512 GiB
  return unlock_max_input_file_bytes.load(std::memory_order_relaxed)
             ? unlocked_cap
             : max_input_file_bytes;
}

inline constexpr std::uint64_t ordinary_large_defer_min_pixels = 10'000'000ull;
inline constexpr std::uint32_t avif_single_image_max_dimension = 65'536u;
inline constexpr std::uint64_t avif_single_image_max_pixels = 1ull << 30;
inline constexpr std::uint64_t ordinary_large_safe_max_pixels =
    avif_single_image_max_pixels;
inline constexpr std::uint32_t svtav1hdr_single_image_max_width = 16'384u;
inline constexpr std::uint32_t svtav1hdr_single_image_max_height = 8'704u;
inline constexpr std::uint64_t svt_safe_max_pixels =
    static_cast<std::uint64_t>(svtav1hdr_single_image_max_width) *
    svtav1hdr_single_image_max_height;
inline constexpr std::uint32_t grid_auto_tile_width = svtav1hdr_single_image_max_width;
inline constexpr std::uint32_t grid_auto_tile_height = svtav1hdr_single_image_max_height;
inline constexpr std::uint32_t grid_max_cols = 256u;
inline constexpr std::uint32_t grid_max_rows = 256u;
inline constexpr bool default_experimental_clamped_grid_padding = false;
inline constexpr std::uint64_t large_image_threshold_pixels =
    ordinary_large_safe_max_pixels;
inline constexpr int default_grid_overlap_pixels = 0;
inline constexpr int default_aom_thread_cap = 8;
inline constexpr int default_av1_encoder_thread_cap = default_aom_thread_cap;
inline constexpr int default_zenrav1e_thread_cap = default_aom_thread_cap;
inline constexpr int default_svtav1hdr_thread_cap = 16;
inline constexpr int default_jxl_thread_cap = 16;
inline constexpr int default_other_encoder_thread_cap = 4;

inline constexpr std::size_t codec_metadata_max_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t jpeg_max_saved_metadata_marker_count = 4096;
inline constexpr std::size_t jpeg_marker_payload_max_bytes = 0xFFFFu - 2u;
inline constexpr std::size_t png_max_cached_metadata_chunks = 64;
inline constexpr std::size_t wic_max_color_contexts = 16;

inline constexpr std::size_t jxl_min_basic_info_probe_bytes = 16 * 1024;
inline constexpr std::size_t jxl_max_basic_info_probe_bytes = 1024 * 1024;
inline constexpr std::size_t jxl_min_encoder_output_buffer_bytes = 16 * 1024;
inline constexpr std::size_t jxl_max_initial_encoder_output_buffer_bytes =
    1024 * 1024;
inline constexpr std::size_t jxl_max_metadata_box_count = 16;

inline constexpr std::size_t webp_min_encoder_output_capacity = 16 * 1024;
inline constexpr std::size_t webp_max_initial_encoder_output_capacity =
    1024 * 1024;

inline constexpr int max_output_temp_path_attempts = 1000;
inline constexpr std::size_t output_copy_buffer_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t svtav1hdr_option_value_max_length = 512;

inline constexpr double visual_gmsd_best = 0.0025;
inline constexpr double visual_gmsd_worst = 0.60;
inline constexpr double visual_gmsd_curve_gamma = 0.70;
inline constexpr double visual_msssim_best = 0.9998;
inline constexpr double visual_msssim_worst = 0.995;
inline constexpr double visual_msssim_curve_gamma = 1.0;
inline constexpr double visual_gmsd_weight = 0.90;
inline constexpr double visual_msssim_weight = 0.10;
inline constexpr int visual_encoder_quality_min = 1;
inline constexpr int visual_encoder_quality_max = 100;
inline constexpr double visual_search_range_max = 34.0;
inline constexpr double visual_search_range_min = 5.0;
inline constexpr double visual_search_center_bias = 5.0;
inline constexpr double visual_search_curve_gamma = 1.28;

inline constexpr bool default_avif_tune_iq = true;
inline constexpr int default_zenrav1e_keyint = 1;
inline constexpr bool default_zenrav1e_still_picture = true;
inline constexpr bool default_zenrav1e_enable_qm = true;
inline constexpr double default_zenrav1e_vaq_strength = 1.0;
inline constexpr bool default_zenrav1e_enable_trellis = false;
inline constexpr bool default_zenrav1e_rdo_tx_decision = false;

}  // namespace awj::encoding_defaults
