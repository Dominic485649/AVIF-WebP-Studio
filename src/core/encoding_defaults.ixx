module;

#include <cstdint>
#include <string_view>

export module awj.encoding_defaults;

export namespace awj::encoding_defaults {

inline constexpr std::string_view default_input_path_text = "input";
inline constexpr std::wstring_view default_input_path = L"input";
inline constexpr std::string_view default_output_template_text = "{name}";
inline constexpr std::wstring_view default_output_template = L"{name}";
inline constexpr std::string_view default_memory_limit_text = "auto";

inline constexpr int default_avif_quality = 90;
inline constexpr int default_webp_quality = 95;
inline constexpr int default_jxl_quality = 95;
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

inline constexpr int default_native_speed = 5;
inline constexpr int default_avif_native_speed = 4;
inline constexpr int default_webp_native_speed = 10;
inline constexpr int default_jxl_native_speed = 4;
inline constexpr int default_jxl_effort = 7;
inline constexpr bool default_jxl_effort_is_explicit = false;
inline constexpr int default_aom_cpu_used = 6;
inline constexpr int default_zenrav1e_preset = 6;
inline constexpr int default_svt_preset = 6;
inline constexpr int default_svtav1hdr_crf = 30;
inline constexpr int default_svtav1hdr_preset = 6;
inline constexpr std::string_view default_svtav1hdr_tune = "3";
inline constexpr int default_svtav1hdr_keyint = 1;
inline constexpr bool default_svtav1hdr_avif = true;

inline constexpr std::uint64_t max_input_file_bytes = 20ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t max_decoded_pixels = 1'000'000'000ull;

inline constexpr std::uint64_t ordinary_large_defer_min_pixels = 10'000'000ull;
inline constexpr std::uint64_t ordinary_large_safe_max_pixels = 30'000'000ull;
inline constexpr std::uint32_t avif_single_image_max_dimension = 65'536u;
inline constexpr std::uint32_t grid_auto_tile_width = 16'384u;
inline constexpr std::uint32_t grid_auto_tile_height = 8'704u;
inline constexpr std::uint32_t grid_max_cols = 256u;
inline constexpr std::uint32_t grid_max_rows = 256u;
inline constexpr bool default_experimental_clamped_grid_padding = true;
inline constexpr std::uint64_t large_image_threshold_pixels = ordinary_large_defer_min_pixels;
inline constexpr std::uint64_t svt_safe_max_pixels = ordinary_large_safe_max_pixels;
inline constexpr int default_grid_overlap_pixels = 0;
inline constexpr int default_av1_encoder_thread_cap = 8;

inline constexpr bool default_avif_tune_iq = true;
inline constexpr int default_zenrav1e_keyint = 1;
inline constexpr bool default_zenrav1e_still_picture = true;
inline constexpr bool default_zenrav1e_enable_qm = true;
inline constexpr double default_zenrav1e_vaq_strength = 1.0;
inline constexpr bool default_zenrav1e_enable_trellis = false;
inline constexpr bool default_zenrav1e_rdo_tx_decision = false;

}  // namespace awj::encoding_defaults
