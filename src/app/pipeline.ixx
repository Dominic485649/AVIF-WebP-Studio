module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <cstdio>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <objbase.h>


export module awj.pipeline;

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.large_image_plan;
import awj.resource_planner;
import awj.visual_quality;
import awj.native_backend;

export namespace awj {

struct BatchLargeImageItem {
  ImageFile file{};
  ImageDimensions dimensions{};
  LargeImageDecision decision{};
};

namespace pipeline_detail {

struct WorkGroup {
  std::uintmax_t weight{};
  std::vector<ImageFile> files{};
};

struct ClassifiedWork {
  std::vector<ImageFile> ordinary{};
  std::vector<ImageFile> deferred_tail{};
  std::vector<BatchLargeImageItem> large_mode{};
};

std::string large_image_action_text(const BatchLargeImageItem& item) {
  std::string actions;
  if (item.decision.available_grid) {
    actions = "grid";
  }
  if (item.decision.available_zenrav1e) {
    actions += actions.empty() ? "zenrav1e" : "/zenrav1e";
  }
  if (actions.empty()) {
    actions = "无可用大图编码器";
  }
  return std::format("[LARGE] {:04} {} ({}x{}, {}) -> 大图模式：{}；{}",
                     item.file.index + 1,
                     path_to_utf8(item.file.path.filename()),
                     item.dimensions.width, item.dimensions.height,
                     large_image_reason_name(item.decision.reason), actions,
                     item.decision.reason_text);
}

bool avif_capability_available_for_large_mode(
    const AvifEncoderCapability& capability) noexcept {
  return capability.enabled &&
         (!capability.experimental || capability.feature_enabled);
}

std::expected<ClassifiedWork, std::string> classify_work_for_avif(
    const AppConfig& cfg,
    const std::vector<ImageFile>& files) {
  ClassifiedWork classified{};
  if (cfg.output_format != OutputFormat::avif) {
    classified.ordinary = files;
    return classified;
  }

  const auto capabilities = avif_encoder_capabilities_for_current_build(
      cfg.enable_experimental_encoders);
  const bool grid_available = std::ranges::any_of(capabilities, [](const auto& capability) {
    return capability.mode == AvifEncoderMode::aom &&
           avif_capability_available_for_large_mode(capability) &&
           capability.supports_avif_grid;
  });
  const bool zenrav1e_available = std::ranges::any_of(capabilities, [](const auto& capability) {
    return capability.mode == AvifEncoderMode::zenrav1e &&
           avif_capability_available_for_large_mode(capability);
  });

  DecoderRegistryOptions decoder_options{.allow_wic_fallback = cfg.allow_wic_fallback};
  for (const auto& image : files) {
    auto dimensions = probe_image_dimensions_for_path(image.path, decoder_options);
    if (!dimensions) {
      return std::unexpected{dimensions.error()};
    }
    auto decision = classify_large_image(*dimensions, grid_available, zenrav1e_available);
    switch (decision.klass) {
      case LargeImageClass::ordinary:
        classified.ordinary.push_back(image);
        break;
      case LargeImageClass::ordinary_deferred_tail:
        classified.deferred_tail.push_back(image);
        break;
      case LargeImageClass::large_mode_required:
        classified.large_mode.push_back(BatchLargeImageItem{.file = image,
                                                            .dimensions = *dimensions,
                                                            .decision = std::move(decision)});
        break;
    }
  }
  return classified;
}

std::vector<WorkGroup> build_work_groups(const AppConfig& cfg,
                                         const std::vector<ImageFile>& files) {
  // 同一输出路径的文件必须串行处理，避免并发覆盖；不同输出路径按总大小分组调度。
  std::vector<WorkGroup> groups;
  std::unordered_map<std::wstring, std::size_t> index_by_output;

  for (const auto& image : files) {
    const auto output = output_path_for(cfg, image);
    auto key = normalized_lower_path_key(output);
    const auto [it, inserted] = index_by_output.emplace(key, groups.size());
    if (inserted) {
      groups.push_back(WorkGroup{});
    }

    auto& group = groups[it->second];
    group.weight += image.bytes;
    group.files.push_back(image);
  }

  // 不同输出路径之间按总大小调度；覆盖/跳过模式下同一路径保留扫描顺序。
  std::ranges::sort(groups, [](const WorkGroup& left, const WorkGroup& right) {
    return left.weight > right.weight;
  });
  return groups;
}

std::string format_result_line(const EncodeResult& result) {
  if (result.ok) {
    if (result.skipped) {
      return std::format("[SKIP] {:04} {} -> 已存在", result.index + 1,
                         path_to_utf8(result.input_path.filename()));
    }

    const double ratio =
        result.original_bytes == 0
            ? 0.0
            : static_cast<double>(result.output_bytes) /
                  static_cast<double>(result.original_bytes);
    if (result.requested_visual_quality) {
      return std::format("[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s, VQ {}→{:.2f}, q{}, {} 次{})",
                         result.index + 1,
                         path_to_utf8(result.input_path.filename()),
                         path_to_utf8(result.output_path.filename()),
                         format_size(result.output_bytes), ratio * 100.0,
                         result.seconds, *result.requested_visual_quality,
                         result.visual_score, result.final_encoder_quality,
                         result.search_attempt_count,
                         result.lossless ? ", lossless" : "");
    }
    return std::format("[ OK ] {:04} {} -> {} ({}, {:.1f}%, {:.2f}s)",
                       result.index + 1,
                       path_to_utf8(result.input_path.filename()),
                       path_to_utf8(result.output_path.filename()),
                       format_size(result.output_bytes), ratio * 100.0,
                       result.seconds);
  }

  if (result.canceled) {
    return std::format("[CANCEL] {:04} {} -> {}", result.index + 1,
                       path_to_utf8(result.input_path.filename()), result.message);
  }

  return std::format("[FAIL] {:04} {} -> {}", result.index + 1,
                     path_to_utf8(result.input_path.filename()), result.message);
}

}  // namespace pipeline_detail

enum class BatchEventKind { message, warning, item_finished, large_image_queued, summary };

struct BatchSummary {
  int ok_count{};
  int failed_count{};
  int canceled_count{};
  int large_image_deferred_count{};
  int large_image_queued_count{};
  std::uintmax_t original_total{};
  std::uintmax_t output_total{};
  bool canceled{};
  int exit_code{};
};

struct BatchProgress {
  BatchEventKind kind{BatchEventKind::message};
  std::size_t completed{};
  std::size_t total{};
  EncodeResult result{};
  BatchLargeImageItem large_image{};
  BatchSummary summary{};
  std::string text{};
};

using ProgressCallback = std::function<void(const BatchProgress&)>;

void emit_progress(const ProgressCallback& progress, const BatchProgress& event);

namespace pipeline_detail {

struct WorkExecutionResult {
  int worker_failures{};
};

WorkExecutionResult encode_work_groups(const AppConfig& cfg,
                                       FileLogger& logger,
                                       const ResourcePlan& resource_plan,
                                       const std::vector<WorkGroup>& work,
                                       std::size_t progress_total,
                                       std::atomic<std::size_t>& completed,
                                       std::vector<EncodeResult>& results,
                                       const ProgressCallback& progress,
                                       std::stop_token stop_token) {
  WorkExecutionResult execution{};
  if (work.empty()) {
    return execution;
  }

  const int jobs = std::max(
      1, std::min<int>(resource_plan.file_parallelism, static_cast<int>(work.size())));
  std::atomic<std::size_t> next{0};
  std::atomic<int> worker_failures{0};

  std::vector<std::jthread> workers;
  workers.reserve(static_cast<std::size_t>(jobs));
  for (int i = 0; i < jobs; ++i) {
    workers.emplace_back([&] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
      struct ComCleanup {
        ~ComCleanup() { CoUninitialize(); }
      } com_cleanup;
      try {
        set_current_thread_low_priority();
        NativeBackend native_backend{cfg, logger, resource_plan};
        while (true) {
          if (stop_token.stop_requested()) {
            break;
          }

          const auto work_index = next.fetch_add(1);
          if (work_index >= work.size()) {
            break;
          }
          const auto& group = work[work_index];
          if (group.files.size() > 1 &&
              cfg.collision_mode == CollisionMode::overwrite) {
            emit_progress(progress, BatchProgress{
                                        .kind = BatchEventKind::warning,
                                        .completed = completed.load(),
                                        .total = progress_total,
                                        .text = std::format(
                                            "[WARN] 输出重名: {} 个输入将依次覆盖 {}",
                                            group.files.size(),
                                            path_to_utf8(
                                                output_path_for(
                                                    cfg, group.files.back())))});
          }
          for (const auto& image : group.files) {
            if (stop_token.stop_requested()) {
              break;
            }
            auto result = native_backend.encode(image, stop_token);
            const auto done = completed.fetch_add(1) + 1;
            BatchProgress event{.kind = BatchEventKind::item_finished,
                                .completed = done,
                                .total = progress_total,
                                .text = format_result_line(result)};
            event.result = result;
            results[result.index] = std::move(result);
            emit_progress(progress, event);
          }
        }
      } catch (const std::exception& ex) {
        worker_failures.fetch_add(1);
        logger.error(std::format("worker failed: {}", ex.what()));
        emit_progress(progress, BatchProgress{
                                    .kind = BatchEventKind::warning,
                                    .completed = completed.load(),
                                    .total = progress_total,
                                    .text = std::format(
                                        "[WARN] 工作线程异常，已停止该线程: {}",
                                        ex.what())});
      } catch (...) {
        worker_failures.fetch_add(1);
        logger.error("worker failed: unknown exception");
        emit_progress(progress, BatchProgress{
                                    .kind = BatchEventKind::warning,
                                    .completed = completed.load(),
                                    .total = progress_total,
                                    .text =
                                        "[WARN] 工作线程异常，已停止该线程: 未知异常"});
      }
    });
  }
  workers.clear();
  execution.worker_failures = worker_failures.load();
  return execution;
}

}  // namespace pipeline_detail

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

void emit_progress(const ProgressCallback& progress, const BatchProgress& event) {
  try {
    if (progress) {
      progress(event);
    }
  } catch (...) {
    // 进度回调不应该影响编码任务；UI 层异常会被吞掉，批处理继续写 CSV。
  }
}

MemoryStatus current_memory_status() noexcept {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return {};
  }
  return MemoryStatus{.total_bytes = status.ullTotalPhys,
                      .available_bytes = status.ullAvailPhys};
}

std::expected<BatchSummary, std::string> run_batch(
    const AppConfig& cfg,
    ProgressCallback progress = {},
    std::stop_token stop_token = {}) {
  try {
    if (auto valid = validate_config(cfg); !valid) {
      return std::unexpected{valid.error()};
    }

    const auto output_dir = output_dir_for(cfg);

    FileLogger logger{output_dir, cfg.write_log};
    logger.info("native backend enabled");
    if (cfg.write_log && !logger.enabled()) {
      const auto text = std::format("[WARN] 日志写入失败: {}",
                                    logger.last_error());
      emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                            .text = text});
    }

    std::vector<ImageFile> files;
    if (auto scanned = scan_images(cfg, files); !scanned) {
      return std::unexpected{scanned.error()};
    }
    if (files.empty()) {
      emit_progress(progress, BatchProgress{
                                  .kind = BatchEventKind::message,
                                  .text =
                                      "未找到图片。支持: jpg, jpeg, png, webp, bmp, "
                                      "tif, tiff, gif, jxl, jp2, heic, heif, avif。"
                                      "请确认输入目录中有支持格式的文件。"});
      return BatchSummary{.exit_code = 0};
    }

    const auto disambiguated_count =
        std::ranges::count_if(files, &ImageFile::extension_disambiguated);
    if (disambiguated_count > 0) {
      const auto text = std::format(
          "[WARN] 同名不同扩展: {} 个输入会自动保留源扩展名，"
          "例如 1.jpg.avif / 1.bmp.avif",
          disambiguated_count);
      logger.warn(text);
      emit_progress(progress, BatchProgress{.kind = BatchEventKind::warning,
                                            .total = files.size(),
                                            .text = text});
    }

    auto classified = pipeline_detail::classify_work_for_avif(cfg, files);
    if (!classified) {
      return std::unexpected{classified.error()};
    }
    auto ordinary_work =
        pipeline_detail::build_work_groups(cfg, classified->ordinary);
    auto deferred_work =
        pipeline_detail::build_work_groups(cfg, classified->deferred_tail);
    const auto largest_file = std::ranges::max(files, {}, &ImageFile::bytes).bytes;
    const auto configured_memory_limit = cfg.memory_limit_bytes == 0
                                             ? automatic_memory_limit(current_memory_status())
                                             : cfg.memory_limit_bytes;
    const auto resource_plan = plan_resources(ResourcePlanRequest{
        .automatic_thread_budget = cfg.max_jobs,
        .file_count = static_cast<int>(std::max<std::size_t>(1, ordinary_work.size())),
        .memory_limit_bytes = configured_memory_limit,
        .estimated_bytes_per_file = largest_file == 0 ? 1 : largest_file,
        .av1_encoder = cfg.output_format == OutputFormat::avif});
    const auto deferred_resource_plan = plan_large_deferred_resources(
        resource_plan,
        static_cast<int>(classified->deferred_tail.size()));
    const int ordinary_jobs = ordinary_work.empty()
                                  ? 0
                                  : std::max(1, std::min<int>(
                                                    resource_plan.file_parallelism,
                                                    static_cast<int>(ordinary_work.size())));
    const int deferred_jobs = deferred_work.empty()
                                  ? 0
                                  : std::max(1, std::min<int>(
                                                    deferred_resource_plan.file_parallelism,
                                                    static_cast<int>(deferred_work.size())));
    emit_progress(progress, BatchProgress{
                                .kind = BatchEventKind::message,
                                .total = files.size(),
                                .text = std::format(
                                    "共 {} 个文件；普通 {}，尾部延后 {}，大图模式 {}。普通并发 {}，延后并发 {}，编码器线程/文件 {}/{}，内存限制 {}。",
                                    files.size(), classified->ordinary.size(),
                                    classified->deferred_tail.size(),
                                    classified->large_mode.size(), ordinary_jobs,
                                    deferred_jobs,
                                    resource_plan.encoder_threads_per_file,
                                    deferred_resource_plan.encoder_threads_per_file,
                                    resource_plan.memory_limit_bytes == 0
                                        ? std::string{"未限制"}
                                        : format_size(resource_plan.memory_limit_bytes))});

    std::vector<EncodeResult> results(files.size());
    for (const auto& image : files) {
      results[image.index] = EncodeResult{.index = image.index,
                                          .input_path = image.path,
                                          .output_path = output_path_for(cfg, image),
                                          .original_bytes = image.bytes,
                                          .quality = cfg.quality,
                                          .requested_visual_quality = cfg.visual_quality,
                                          .gmsd_weight = GMSD_WEIGHT,
                                          .msssim_weight = MSSSIM_WEIGHT,
                                          .final_encoder_quality = cfg.quality,
                                          .speed = cfg.speed.value_or(-1),
                                          .quality_overridden_by_visual_quality = cfg.visual_quality.has_value(),
                                          .message = "未处理。"};
    }

    for (const auto& item : classified->large_mode) {
      auto& result = results[item.file.index];
      result.processed = false;
      result.large_image_queued = true;
      result.message = "大图模式队列。";
    }

    std::atomic<std::size_t> completed{0};
    int worker_failures = 0;

    const auto ordinary_execution = pipeline_detail::encode_work_groups(
        cfg, logger, resource_plan, ordinary_work, files.size(), completed, results,
        progress, stop_token);
    worker_failures += ordinary_execution.worker_failures;

    if (!stop_token.stop_requested() && !deferred_work.empty()) {
      emit_progress(progress, BatchProgress{
                                  .kind = BatchEventKind::message,
                                  .completed = completed.load(),
                                  .total = files.size(),
                                  .text = std::format(
                                      "开始处理 AVIF 尾部延后队列：{} 个文件，并发 {}，编码器线程/文件 {}。",
                                      classified->deferred_tail.size(), deferred_jobs,
                                      deferred_resource_plan.encoder_threads_per_file)});
      const auto deferred_execution = pipeline_detail::encode_work_groups(
          cfg, logger, deferred_resource_plan, deferred_work, files.size(), completed,
          results, progress, stop_token);
      worker_failures += deferred_execution.worker_failures;
    }

    if (!stop_token.stop_requested()) {
      for (const auto& item : classified->large_mode) {
        const auto text = pipeline_detail::large_image_action_text(item);
        logger.info(text);
        const auto done = completed.fetch_add(1) + 1;
        emit_progress(progress, BatchProgress{.kind = BatchEventKind::large_image_queued,
                                              .completed = done,
                                              .total = files.size(),
                                              .large_image = item,
                                              .text = text});
      }
    }

    std::uintmax_t original_total = 0;
    std::unordered_map<std::wstring, std::uintmax_t> final_output_sizes;
    int ok_count = 0;
    int failed_count = 0;
    int canceled_count = 0;
    const bool canceled = stop_token.stop_requested();
    for (const auto& result : results) {
      const bool large_mode_item = result.large_image_queued;
      if (large_mode_item) {
        continue;
      }
      if (result.ok) {
        ++ok_count;
        original_total += result.original_bytes;
        final_output_sizes[normalized_lower_path_key(result.output_path)] =
            result.output_bytes;
      } else if (result.canceled || (!result.processed && canceled)) {
        ++canceled_count;
      } else {
        ++failed_count;
        if (result.processed) {
          original_total += result.original_bytes;
        }
      }
    }
    std::uintmax_t output_total = 0;
    for (const auto& [_, bytes] : final_output_sizes) {
      output_total += bytes;
    }

    std::string summary_warning;
    if (cfg.write_summary) {
      std::vector<EncodeResult> report_results;
      report_results.reserve(results.size());
      std::ranges::copy_if(results, std::back_inserter(report_results),
                           [&](const EncodeResult& result) {
                             return !result.large_image_queued;
                           });
      if (auto csv = write_csv(output_dir, report_results); !csv) {
        summary_warning = std::format("\n[WARN] {}", csv.error());
        logger.warn(csv.error());
      }
    }
    const double total_ratio =
        original_total == 0
            ? 0.0
            : static_cast<double>(output_total) /
                  static_cast<double>(original_total);
    std::string summary_report;
    if (cfg.write_summary) {
      summary_report = std::format("，报告 {}", path_to_utf8(output_dir / L"summary.csv"));
      if (!summary_warning.empty()) {
        summary_report += "，报告写入失败";
      }
    }

    const int worker_failure_count = worker_failures;
    const bool summary_failed = !summary_warning.empty();
    const bool has_failures =
        failed_count > 0 || worker_failure_count > 0 || summary_failed;

    BatchSummary summary{.ok_count = ok_count,
                         .failed_count = failed_count,
                         .canceled_count = canceled_count,
                         .large_image_deferred_count = static_cast<int>(classified->deferred_tail.size()),
                         .large_image_queued_count = static_cast<int>(classified->large_mode.size()),
                         .original_total = original_total,
                         .output_total = output_total,
                         .canceled = canceled,
                         .exit_code = canceled ? 130 : (has_failures ? 2 : 0)};
    emit_progress(progress, BatchProgress{
                                .kind = BatchEventKind::summary,
                                .completed = completed.load(),
                                .total = files.size(),
                                .summary = summary,
                                .text = std::format(
                                    "{}：成功 {}，失败 {}，取消 {}，大图模式 {}；体积 {} -> {} ({:.1f}%){}",
                                    canceled ? "已取消" : "完成", ok_count,
                                    failed_count, canceled_count,
                                    classified->large_mode.size(),
                                    format_size(original_total),
                                    format_size(output_total), total_ratio * 100.0,
                                    summary_report)});
    return summary;
  } catch (const std::exception& ex) {
    return std::unexpected{std::string{ex.what()}};
  } catch (...) {
    return std::unexpected{"未知异常，程序已安全退出。"};
  }
}

// 顶层流水线返回进程退出码；单张图片错误会落到 CSV，不会让程序闪退。
int run_pipeline(const AppConfig& cfg) {
  std::mutex print_mutex;
  const auto summary = run_batch(
      cfg,
      [&](const BatchProgress& event) {
        std::scoped_lock lock{print_mutex};
        if (event.kind == BatchEventKind::summary) {
          print_line("");
        }
        print_line(event.text);
      });
  if (!summary) {
    print_line(std::format("[FAIL] {}", summary.error()));
    return 1;
  }
  return summary->exit_code;
}

}  // namespace awj
