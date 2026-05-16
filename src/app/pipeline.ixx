module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <cstdio>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

export module avif.pipeline;

import avif.config;
import avif.core;
import avif.magick_backend;

export namespace avif {

namespace pipeline_detail {

struct WorkGroup {
  std::uintmax_t weight{};
  std::vector<ImageFile> files{};
};

std::vector<WorkGroup> build_work_groups(const AppConfig& cfg,
                                         const std::vector<ImageFile>& files) {
  // 同一输出路径的文件必须串行处理，避免并发覆盖；不同输出路径按总大小分组调度。
  std::vector<WorkGroup> groups;
  std::unordered_map<std::wstring, std::size_t> index_by_output;

  for (const auto& image : files) {
    const auto output = output_path_for(cfg, image);
    auto key = normalized_lower_path_key(output);
    if (cfg.collision_mode == CollisionMode::suffix_time ||
        cfg.collision_mode == CollisionMode::suffix_random) {
      key += std::format(L"#{}", image.index);
    }
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

enum class BatchEventKind { message, warning, item_finished, summary };

struct BatchSummary {
  int ok_count{};
  int failed_count{};
  int canceled_count{};
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
  BatchSummary summary{};
  std::string text{};
};

using ProgressCallback = std::function<void(const BatchProgress&)>;

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

void emit_progress(const ProgressCallback& progress, BatchProgress event) {
  try {
    if (progress) {
      progress(event);
    }
  } catch (...) {
    // 进度回调不应该影响编码任务；UI 层异常会被吞掉，批处理继续写 CSV。
  }
}

std::expected<BatchSummary, std::string> run_batch(
    const AppConfig& cfg,
    ProgressCallback progress = {},
    std::stop_token stop_token = {}) {
  try {
    if (auto valid = validate_config(cfg); !valid) {
      return std::unexpected{valid.error()};
    }

    const auto runtime = resolve_magick_runtime(cfg);
    if (!runtime) {
      return std::unexpected{runtime.error()};
    }

    configure_magick_environment(*runtime);
    const auto output_dir = output_dir_for(cfg);

    FileLogger logger{output_dir, cfg.write_log};
    logger.info(std::format("imagemagick runtime: {}", path_to_utf8(runtime->root)));

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

    auto work = pipeline_detail::build_work_groups(cfg, files);
    const int jobs = std::max(
        1, std::min<int>(cfg.max_jobs, static_cast<int>(work.size())));
    emit_progress(progress, BatchProgress{
                                .kind = BatchEventKind::message,
                                .total = files.size(),
                                .text = std::format("共 {} 个文件，并发 {}。", files.size(), jobs)});

    std::vector<EncodeResult> results(files.size());
    for (const auto& image : files) {
      results[image.index] = EncodeResult{.index = image.index,
                                          .input_path = image.path,
                                          .output_path = output_path_for(cfg, image),
                                          .original_bytes = image.bytes,
                                          .quality = cfg.quality,
                                          .speed = cfg.magick_speed.value_or(-1),
                                          .message = "未处理。"};
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> completed{0};

    // 工作线程使用 std::jthread：异常路径或提前返回时也会自动 join，停止请求由 stop_token 传播。
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(jobs));
    for (int i = 0; i < jobs; ++i) {
      workers.emplace_back([&] {
        set_current_thread_low_priority();
        MagickBackend backend{cfg, *runtime, logger};
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
                                        .total = files.size(),
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
            auto result = backend.encode(image, stop_token);
            const auto event_result = result;
            results[result.index] = std::move(result);
            const auto done = completed.fetch_add(1) + 1;
            emit_progress(progress, BatchProgress{
                                        .kind = BatchEventKind::item_finished,
                                        .completed = done,
                                        .total = files.size(),
                                        .result = event_result,
                                        .text = pipeline_detail::format_result_line(
                                            event_result)});
          }
        }
      });
    }

    workers.clear();

    std::uintmax_t original_total = 0;
    std::unordered_map<std::wstring, std::uintmax_t> final_output_sizes;
    int ok_count = 0;
    int failed_count = 0;
    int canceled_count = 0;
    const bool canceled = stop_token.stop_requested();
    for (const auto& result : results) {
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
      if (auto csv = write_csv(output_dir, results); !csv) {
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

    BatchSummary summary{.ok_count = ok_count,
                         .failed_count = failed_count,
                         .canceled_count = canceled_count,
                         .original_total = original_total,
                         .output_total = output_total,
                         .canceled = canceled,
                         .exit_code = canceled ? 130 : (failed_count == 0 ? 0 : 2)};
    emit_progress(progress, BatchProgress{
                                .kind = BatchEventKind::summary,
                                .completed = completed.load(),
                                .total = files.size(),
                                .summary = summary,
                                .text = std::format(
                                    "{}：成功 {}，失败 {}，取消 {}；体积 {} -> {} ({:.1f}%){}",
                                    canceled ? "已取消" : "完成", ok_count,
                                    failed_count, canceled_count,
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

}  // namespace avif
