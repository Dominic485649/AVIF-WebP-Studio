module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>

export module awj.resource_planner;

import awj.encoding_defaults;

export namespace awj {

struct MemoryStatus {
  std::uint64_t total_bytes{};
  std::uint64_t available_bytes{};
};

struct ResourcePlanRequest {
  int automatic_thread_budget{1};
  int file_count{1};
  std::uint64_t memory_limit_bytes{};
  std::uint64_t estimated_bytes_per_file{};
  bool force_single_thread_per_file{};
};

struct ResourcePlan {
  int file_parallelism{1};
  int encoder_threads_per_file{1};
  int global_thread_budget{1};
  std::uint64_t memory_limit_bytes{};
  int memory_file_parallelism{1};
};

std::uint64_t automatic_memory_limit(MemoryStatus status) noexcept {
  const auto half_total = status.total_bytes / 2;
  const auto available_headroom = static_cast<std::uint64_t>(
      static_cast<long double>(status.available_bytes) * 0.8L);
  if (half_total == 0) {
    return available_headroom;
  }
  if (available_headroom == 0) {
    return half_total;
  }
  return std::min(half_total, available_headroom);
}

int exact_file_parallelism(int budget, int desired) noexcept {
  const int clamped_desired = std::clamp(desired, 1, budget);
  for (int parallelism = clamped_desired; parallelism >= 1; --parallelism) {
    if (budget % parallelism == 0) {
      return parallelism;
    }
  }
  return 1;
}

ResourcePlan plan_resources(ResourcePlanRequest request) noexcept {
  const int budget = std::max(1, request.automatic_thread_budget);
  const int files = std::max(1, request.file_count);
  const std::uint64_t memory_limit = request.memory_limit_bytes;
  const std::uint64_t per_file = std::max<std::uint64_t>(1, request.estimated_bytes_per_file);

  int memory_parallelism = budget;
  if (memory_limit > 0) {
    const auto memory_bound = std::max<std::uint64_t>(1, memory_limit / per_file);
    memory_parallelism = static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(budget), memory_bound));
  }

  constexpr int single_thread_batch_threshold = 12;
  const bool single_thread_per_file = request.force_single_thread_per_file ||
                                      files > single_thread_batch_threshold;
  const int file_parallelism = single_thread_per_file
                                   ? budget
                                   : exact_file_parallelism(
                                         budget, std::min(files, budget));
  const int encoder_threads = budget / file_parallelism;

  return ResourcePlan{.file_parallelism = file_parallelism,
                      .encoder_threads_per_file = encoder_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = memory_limit,
                      .memory_file_parallelism = memory_parallelism};
}

ResourcePlan plan_large_deferred_resources(ResourcePlan base,
                                                   int file_count) noexcept {
  static_cast<void>(file_count);
  return ResourcePlan{.file_parallelism = std::max(1, base.file_parallelism),
                      .encoder_threads_per_file = std::max(1, base.encoder_threads_per_file),
                      .global_thread_budget = std::max(1, base.global_thread_budget),
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = std::max(1, base.memory_file_parallelism)};
}

ResourcePlan plan_grid_encode_resources(ResourcePlan base,
                                               int tile_count) noexcept {
  const int budget = std::max(1, base.encoder_threads_per_file);
  const int tiles = std::max(1, tile_count);
  const int tile_parallelism = exact_file_parallelism(
      budget, std::min({tiles, budget, std::max(1, base.global_thread_budget)}));
  const int per_tile_threads = budget / tile_parallelism;
  return ResourcePlan{.file_parallelism = tile_parallelism,
                      .encoder_threads_per_file = per_tile_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = tile_parallelism};
}

ResourcePlan plan_large_mode_resources(ResourcePlan base,
                                               int file_count,
                                               std::uint64_t largest_working_set_bytes) noexcept {
  const int budget = std::max(1, base.global_thread_budget);
  static_cast<void>(file_count);
  const int threads_per_file = std::max(1, base.encoder_threads_per_file);
  int memory_parallelism = std::max(1, base.memory_file_parallelism);
  if (base.memory_limit_bytes > 0) {
    const std::uint64_t per_file = std::max<std::uint64_t>(1, largest_working_set_bytes);
    const auto memory_bound = std::max<std::uint64_t>(1, base.memory_limit_bytes / per_file);
    memory_parallelism = static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(budget), memory_bound));
  }
  return ResourcePlan{.file_parallelism = std::max(1, base.file_parallelism),
                      .encoder_threads_per_file = threads_per_file,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = memory_parallelism};
}

}  // namespace awj
