#include <cstdio>
#include <string>
#include <string_view>

import awj.codec;
import awj.encoding_defaults;
import awj.resource_planner;

namespace {

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

}  // namespace

int main() {
  const auto memory = awj::automatic_memory_limit(
      awj::MemoryStatus{.total_bytes = 32ull * 1024ull * 1024ull * 1024ull,
                         .available_bytes = 10ull * 1024ull * 1024ull * 1024ull});
  if (memory != 8ull * 1024ull * 1024ull * 1024ull) {
    return fail("自动内存限制未使用 min(total/2, available*0.8)。");
  }

  const auto single_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_aom_thread_cap});
  constexpr int single_av1_expected_threads =
      awj::encoding_defaults::default_aom_thread_cap;
  if (single_av1.file_parallelism != 1 ||
      single_av1.encoder_threads_per_file != single_av1_expected_threads) {
    return fail("单文件 AOM 未按线程 cap 规划 encoder 线程。");
  }

  const auto low_budget_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 6,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_aom_thread_cap});
  if (low_budget_av1.file_parallelism != 1 ||
      low_budget_av1.encoder_threads_per_file != 6) {
    return fail("单文件 AOM 低预算不应额外扣减线程。");
  }

  const auto batch_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 100,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_aom_thread_cap});
  if (batch_av1.file_parallelism * batch_av1.encoder_threads_per_file > 12 ||
      batch_av1.encoder_threads_per_file != 1) {
    return fail("大批量 AV1 未降低 encoder 内部线程。");
  }

  const auto jxl_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_jxl_thread_cap});
  if (jxl_single.file_parallelism != 1 ||
      jxl_single.encoder_threads_per_file !=
          awj::encoding_defaults::default_jxl_thread_cap) {
    return fail("单文件 JXL 未放宽到 JXL 线程 cap。");
  }

  const auto svt_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_svtav1hdr_thread_cap});
  if (svt_single.file_parallelism != 1 ||
      svt_single.encoder_threads_per_file !=
          awj::encoding_defaults::default_svtav1hdr_thread_cap) {
    return fail("单文件 SVT 未放宽到 SVT 线程 cap。");
  }

  const auto memory_limited = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 12,
                                .memory_limit_bytes = 300,
                                .estimated_bytes_per_file = 128,
                                .encoder_thread_cap =
                                    awj::encoding_defaults::default_aom_thread_cap});
  if (memory_limited.file_parallelism != 2) {
    return fail("内存预算未限制文件并行数。");
  }

  const auto grid_resources = awj::plan_grid_encode_resources(
      awj::ResourcePlan{.file_parallelism = 1,
                         .encoder_threads_per_file = 8,
                         .global_thread_budget = 12,
                         .memory_limit_bytes = 0,
                         .memory_file_parallelism = 1},
      4);
  if (grid_resources.file_parallelism != 4 ||
      grid_resources.encoder_threads_per_file != 2 ||
      grid_resources.file_parallelism * grid_resources.encoder_threads_per_file > 8) {
    return fail("AVIF grid 线程预算未拆分为 tile 并行与 per-tile 线程。");
  }

  const auto large_resources = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 4,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 12,
                         .memory_limit_bytes = 900,
                         .memory_file_parallelism = 4},
      8, 300);
  if (large_resources.file_parallelism != 3 ||
      large_resources.encoder_threads_per_file != 4 ||
      large_resources.file_parallelism * large_resources.encoder_threads_per_file > 12) {
    return fail("大图模式资源规划未同时约束线程预算和内存预算。");
  }

  const auto large_mode_aom_cap = awj::plan_large_mode_resources(
      awj::plan_resources(
          awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                   .file_count = 1,
                                   .memory_limit_bytes = 0,
                                   .estimated_bytes_per_file = 1,
                                   .encoder_thread_cap =
                                       awj::encoding_defaults::default_aom_thread_cap}),
      1, 1);
  if (large_mode_aom_cap.encoder_threads_per_file !=
      awj::encoding_defaults::default_aom_thread_cap) {
    return fail("大图模式应按 AOM/grid 可用线程 cap 规划。");
  }

  const auto large_mode_memory_tight = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 8,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 16,
                         .memory_limit_bytes = 700,
                         .memory_file_parallelism = 8},
      4, 300);
  if (large_mode_memory_tight.file_parallelism != 2) {
    return fail("大图模式内存预算未继续约束文件并发。");
  }

  const auto grid_single_tile = awj::plan_grid_encode_resources(
      awj::ResourcePlan{.file_parallelism = 1,
                         .encoder_threads_per_file = 3,
                         .global_thread_budget = 3,
                         .memory_limit_bytes = 0,
                         .memory_file_parallelism = 1},
      1);
  if (grid_single_tile.file_parallelism != 1 ||
      grid_single_tile.encoder_threads_per_file != 3) {
    return fail("单 tile grid 不应拆分出额外并行开销。");
  }

  const auto avif_speed = awj::map_speed_for_format(awj::OutputFormat::avif, 10);
  const auto webp_speed = awj::map_speed_for_format(awj::OutputFormat::webp, 10);
  const auto jxl_speed = awj::map_speed_for_format(awj::OutputFormat::jxl, 10);
  const auto jpegli_speed = awj::map_speed_for_format(awj::OutputFormat::jpgli, 10);
  if (avif_speed.codec_value != 0 || webp_speed.codec_value != 0 ||
      jxl_speed.codec_value != 1 || jpegli_speed.codec_value != -1 ||
      !jpegli_speed.codec_key.empty()) {
    return fail("speed=10 未映射到最快 codec 档位。");
  }

  return 0;
}
