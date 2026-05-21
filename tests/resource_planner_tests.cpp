#include <cstdio>
#include <string_view>

import awj.codec;
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
                                .av1_encoder = true});
  if (single_av1.file_parallelism != 1 ||
      single_av1.encoder_threads_per_file != 12) {
    return fail("单文件 AV1 未获得完整自动线程预算。");
  }

  const auto batch_av1 = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 100,
                                .memory_limit_bytes = 0,
                                .estimated_bytes_per_file = 1,
                                .av1_encoder = true});
  if (batch_av1.file_parallelism * batch_av1.encoder_threads_per_file > 12 ||
      batch_av1.encoder_threads_per_file != 1) {
    return fail("大批量 AV1 未降低 encoder 内部线程。");
  }

  const auto memory_limited = awj::plan_resources(
      awj::ResourcePlanRequest{.automatic_thread_budget = 12,
                                .file_count = 12,
                                .memory_limit_bytes = 300,
                                .estimated_bytes_per_file = 128,
                                .av1_encoder = true});
  if (memory_limited.file_parallelism != 2) {
    return fail("内存预算未限制文件并行数。");
  }

  const auto avif_speed = awj::map_speed_for_format(awj::OutputFormat::avif, 10);
  const auto webp_speed = awj::map_speed_for_format(awj::OutputFormat::webp, 10);
  const auto jxl_speed = awj::map_speed_for_format(awj::OutputFormat::jxl, 10);
  if (avif_speed.codec_value != 0 || webp_speed.codec_value != 0 ||
      jxl_speed.codec_value != 3) {
    return fail("speed=10 未映射到最快 codec 档位。");
  }

  return 0;
}
