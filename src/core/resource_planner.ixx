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
  int encoder_thread_cap{encoding_defaults::default_other_encoder_thread_cap};
};

struct ResourcePlan {
  int file_parallelism{1};
  int encoder_threads_per_file{1};
  int global_thread_budget{1};
  std::uint64_t memory_limit_bytes{};
  int memory_file_parallelism{1};
};

export std::uint64_t automatic_memory_limit(MemoryStatus status) noexcept {
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

export ResourcePlan plan_resources(ResourcePlanRequest request) noexcept {
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

  int file_parallelism = std::clamp(std::min(files, budget), 1, memory_parallelism);

  int encoder_threads = std::max(1, budget / file_parallelism);
  const int encoder_cap = std::max(1, request.encoder_thread_cap);
  encoder_threads = std::min(encoder_threads, encoder_cap);
  while (file_parallelism * encoder_threads > budget && encoder_threads > 1) {
    --encoder_threads;
  }

  return ResourcePlan{.file_parallelism = file_parallelism,
                      .encoder_threads_per_file = encoder_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = memory_limit,
                      .memory_file_parallelism = memory_parallelism};
}

export ResourcePlan plan_large_deferred_resources(ResourcePlan base,
                                                   int file_count) noexcept {
  const int budget = std::max(1, base.global_thread_budget);
  const int threads_per_file =
      std::max(1, std::min({base.encoder_threads_per_file, budget}));
  const int files = std::max(1, file_count);
  const int memory_parallelism = std::max(1, base.memory_file_parallelism);
  const int file_parallelism = std::max(
      1, std::min({files, budget / threads_per_file, memory_parallelism}));
  return ResourcePlan{.file_parallelism = file_parallelism,
                      .encoder_threads_per_file = threads_per_file,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = memory_parallelism};
}

export ResourcePlan plan_grid_encode_resources(ResourcePlan base,
                                               int tile_count) noexcept {
  const int budget = std::max(1, base.encoder_threads_per_file);
  const int tiles = std::max(1, tile_count);
  const int tile_parallelism =
      std::max(1, std::min({tiles, budget, base.global_thread_budget}));
  const int per_tile_threads = std::max(1, budget / tile_parallelism);
  return ResourcePlan{.file_parallelism = tile_parallelism,
                      .encoder_threads_per_file = per_tile_threads,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = tile_parallelism};
}

export ResourcePlan plan_large_mode_resources(ResourcePlan base,
                                               int file_count,
                                               std::uint64_t largest_working_set_bytes) noexcept {
  const int budget = std::max(1, base.global_thread_budget);
  const int files = std::max(1, file_count);
  const int threads_per_file =
      std::max(1, std::min({base.encoder_threads_per_file, budget}));
  int memory_parallelism = std::max(1, base.memory_file_parallelism);
  if (base.memory_limit_bytes > 0) {
    const std::uint64_t per_file = std::max<std::uint64_t>(1, largest_working_set_bytes);
    const auto memory_bound = std::max<std::uint64_t>(1, base.memory_limit_bytes / per_file);
    memory_parallelism = static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(budget), memory_bound));
  }
  const int file_parallelism = std::max(
      1, std::min({files, budget / threads_per_file, memory_parallelism}));
  return ResourcePlan{.file_parallelism = file_parallelism,
                      .encoder_threads_per_file = threads_per_file,
                      .global_thread_budget = budget,
                      .memory_limit_bytes = base.memory_limit_bytes,
                      .memory_file_parallelism = memory_parallelism};
}

}  // namespace awj
