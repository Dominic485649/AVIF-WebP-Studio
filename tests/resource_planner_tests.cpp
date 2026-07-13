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
                                .estimated_bytes_per_file = 1});
  if (single_av1.file_parallelism != 1 ||
      single_av1.encoder_threads_per_file != 12) {
    return fail("单文件未把完整自动线程预算传给 encoder。");
  }

  const auto low_budget_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 6,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (low_budget_av1.file_parallelism != 1 ||
      low_budget_av1.encoder_threads_per_file != 6) {
    return fail("单文件 AOM 低预算不应额外扣减线程。");
  }

  const auto batch_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 100,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (batch_av1.file_parallelism != 12 ||
      batch_av1.file_parallelism * batch_av1.encoder_threads_per_file != 12 ||
      batch_av1.encoder_threads_per_file != 1) {
    return fail("大批量 AV1 未降低 encoder 内部线程。");
  }

  const auto wide_batch = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 28,
                                .file_count = 13,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (wide_batch.file_parallelism != 28 ||
      wide_batch.encoder_threads_per_file != 1 ||
      wide_batch.file_parallelism * wide_batch.encoder_threads_per_file != 28) {
    return fail("超过 12 张图片时必须按单线程并发规划。");
  }

  const auto three_files = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 8,
                                .file_count = 3,
                                .estimated_bytes_per_file = 1});
  const auto five_files = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 5,
                                .estimated_bytes_per_file = 1});
  const auto prime_budget = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 7,
                                .file_count = 3,
                                .estimated_bytes_per_file = 1});
  const auto threshold_batch = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 28,
                                .file_count = 12,
                                .estimated_bytes_per_file = 1});
  const auto forced_stage = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 28,
                                .file_count = 5,
                                .estimated_bytes_per_file = 1,
                                .force_single_thread_per_file = true});
  if (three_files.file_parallelism != 2 || three_files.encoder_threads_per_file != 4 ||
      five_files.file_parallelism != 4 || five_files.encoder_threads_per_file != 3 ||
      prime_budget.file_parallelism != 1 || prime_budget.encoder_threads_per_file != 7 ||
      threshold_batch.file_parallelism != 7 ||
      threshold_batch.encoder_threads_per_file != 4 ||
      forced_stage.file_parallelism != 28 ||
      forced_stage.encoder_threads_per_file != 1) {
    return fail("线程预算未精确拆分为 encoder 线程与文件并发。");
  }

  const auto jxl_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (jxl_single.file_parallelism != 1 ||
      jxl_single.encoder_threads_per_file != 20) {
    return fail("单文件 JXL 未收到完整线程预算。");
  }

  const auto svt_single = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                .file_count = 1,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1});
  if (svt_single.file_parallelism != 1 ||
      svt_single.encoder_threads_per_file != 20) {
    return fail("单文件 SVT 未收到完整线程预算。");
  }

  const auto memory_limited = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 12,
                                .memory_limit_bytes = 300,
                                .estimated_bytes_per_file = 128});
  if (memory_limited.file_parallelism != 12 ||
      memory_limited.encoder_threads_per_file != 1 ||
      memory_limited.memory_file_parallelism != 2) {
    return fail("内存并发限制不应破坏 CPU 线程预算乘积。");
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
      grid_resources.file_parallelism * grid_resources.encoder_threads_per_file != 8) {
    return fail("AVIF grid 线程预算未拆分为 tile 并行与 per-tile 线程。");
  }

  const auto large_resources = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 3,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 12,
                         .memory_limit_bytes = 900,
                         .memory_file_parallelism = 4},
      8, 300);
  if (large_resources.file_parallelism != 3 ||
      large_resources.encoder_threads_per_file != 4 ||
      large_resources.file_parallelism * large_resources.encoder_threads_per_file != 12 ||
      large_resources.memory_file_parallelism != 3) {
    return fail("大图模式资源规划未同时约束线程预算和内存预算。");
  }

  const auto large_mode_single = awj::plan_large_mode_resources(
      awj::plan_resources(
          awj::ResourcePlanRequest{.automatic_thread_budget = 20,
                                   .file_count = 1,
                                   .memory_limit_bytes = 0,
                                   .estimated_bytes_per_file = 1}),
      1, 1);
  if (large_mode_single.file_parallelism != 1 ||
      large_mode_single.encoder_threads_per_file != 20) {
    return fail("单个大图未保留完整线程预算。");
  }

  const auto large_mode_memory_tight = awj::plan_large_mode_resources(
      awj::ResourcePlan{.file_parallelism = 4,
                         .encoder_threads_per_file = 4,
                         .global_thread_budget = 16,
                         .memory_limit_bytes = 700,
                         .memory_file_parallelism = 8},
      4, 300);
  if (large_mode_memory_tight.file_parallelism != 4 ||
      large_mode_memory_tight.memory_file_parallelism != 2) {
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
