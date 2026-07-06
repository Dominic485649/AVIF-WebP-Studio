#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <windows.h>

import awj.config;
import awj.core;
import awj.image;
import awj.pipeline;
import awj.resource_planner;
import awj.webp_codec;

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::byte red = std::byte{255}) {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      red,          std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0}, std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                         .height = 2,
                         .pixel_format = awj::PixelFormat::rgba,
                         .alpha_mode = awj::AlphaMode::straight,
                         .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

std::expected<void, std::string> write_webp(const std::filesystem::path& path,
                                            std::byte red = std::byte{255}) {
  awj::WebPImageEncoder encoder;
  auto encoded = encoder.encode(
      make_test_image(red),
      awj::NativeEncodeSettings{.output_format = awj::OutputFormat::webp,
                                .quality = 100,
                                .speed = 10,
                                .resources = awj::ResourcePlan{
                                    .file_parallelism = 1,
                                    .encoder_threads_per_file = 1,
                                    .global_thread_budget = 1}});
  if (!encoded) {
    return std::unexpected{encoded.error()};
  }
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(encoded->encoded.bytes.data()),
               static_cast<std::streamsize>(encoded->encoded.bytes.size()));
  if (!stream) {
    return std::unexpected{"failed to write test webp."};
  }
  return {};
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "awjimage-pipeline-security-tests";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return fail("failed to create temp root.");
  }

  auto missing_cfg = awj::default_app_config();
  missing_cfg.input_path = root / "missing.webp";
  auto missing_summary = awj::run_batch(missing_cfg);
  if (missing_summary || missing_summary.error().find("不存在") == std::string::npos) {
    return fail("missing input path was not rejected clearly.");
  }

  const auto input_dir = root / "input";
  const auto output_dir = root / "out";
  std::filesystem::create_directories(input_dir, ec);
  if (ec) {
    return fail("failed to create input dir.");
  }
  if (auto ok = write_webp(input_dir / "same-a.webp", std::byte{255}); !ok) {
    return fail(ok.error());
  }
  if (auto ok = write_webp(input_dir / "same-b.webp", std::byte{128}); !ok) {
    return fail(ok.error());
  }

  awj::AppConfig cfg = awj::default_app_config();
  cfg.input_path = input_dir;
  cfg.output_dir = output_dir;
  cfg.output_format = awj::OutputFormat::webp;
  cfg.output_template = L"same";
  cfg.collision_mode = awj::CollisionMode::suffix_random;
  cfg.max_jobs = 2;
  cfg.quality = 100;
  cfg.write_summary = true;
  auto summary = awj::run_batch(cfg);
  if (!summary || summary->ok_count != 2 || summary->failed_count != 0) {
    return fail(summary ? "suffix collision batch did not succeed." : summary.error());
  }

  std::vector<std::filesystem::path> outputs;
  for (const auto& entry : std::filesystem::directory_iterator{output_dir}) {
    if (entry.path().extension() == ".webp") {
      outputs.push_back(entry.path());
    }
  }
  if (outputs.size() != 2) {
    return fail("suffix collision did not create two distinct outputs.");
  }

  const auto numbered_dir = root / "numbered";
  std::filesystem::create_directories(numbered_dir, ec);
  if (ec) {
    return fail("failed to create numbered dir.");
  }
  const auto numbered_input = numbered_dir / "name.webp";
  if (auto ok = write_webp(numbered_input, std::byte{32}); !ok) {
    return fail(ok.error());
  }
  awj::AppConfig number_cfg = awj::default_app_config();
  number_cfg.input_path = numbered_input;
  number_cfg.output_dir = numbered_dir;
  number_cfg.output_format = awj::OutputFormat::webp;
  number_cfg.collision_mode = awj::CollisionMode::suffix_number;
  number_cfg.max_jobs = 1;
  number_cfg.quality = 100;
  auto number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(1).webp")) {
    return fail(number_summary ? "numbered collision did not create name(1)."
                               : number_summary.error());
  }
  number_cfg.input_path = numbered_dir / "name(1).webp";
  number_summary = awj::run_batch(number_cfg);
  if (!number_summary || number_summary->ok_count != 1 ||
      !std::filesystem::exists(numbered_dir / "name(2).webp") ||
      std::filesystem::exists(numbered_dir / "name(1)(1).webp")) {
    return fail(number_summary ? "numbered collision did not advance existing suffix."
                               : number_summary.error());
  }

  const auto short_dir = root / "short-path";
  std::filesystem::create_directories(short_dir, ec);
  if (ec) {
    return fail("failed to create short-path dir.");
  }
  const auto long_webp = short_dir / "20B07A very long filename that must stay complete.webp";
  if (auto ok = write_webp(long_webp, std::byte{16}); !ok) {
    return fail(ok.error());
  }
  const DWORD needed = GetShortPathNameW(long_webp.c_str(), nullptr, 0);
  if (needed > 0) {
    std::wstring short_buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written = GetShortPathNameW(
        long_webp.c_str(), short_buffer.data(), static_cast<DWORD>(short_buffer.size()));
    if (written > 0 && written < short_buffer.size()) {
      short_buffer.resize(written);
      const std::filesystem::path short_webp{short_buffer};
      if (short_webp.native() != long_webp.native()) {
        const auto short_out = root / "short-out";
        awj::AppConfig short_cfg = awj::default_app_config();
        short_cfg.input_path = short_webp;
        short_cfg.output_dir = short_out;
        short_cfg.output_format = awj::OutputFormat::webp;
        short_cfg.collision_mode = awj::CollisionMode::suffix_number;
        short_cfg.max_jobs = 1;
        short_cfg.quality = 100;
        auto short_summary = awj::run_batch(short_cfg);
        if (!short_summary || short_summary->ok_count != 1 ||
            !std::filesystem::exists(
                short_out / "20B07A very long filename that must stay complete.webp")) {
          return fail(short_summary ? "short WEBP path did not keep long output name."
                                    : short_summary.error());
        }
      }
    }
  }

  const auto csv = read_text(output_dir / "summary.csv");
  if (csv.find("same-a.webp") == std::string::npos ||
      csv.find(root.string()) != std::string::npos) {
    return fail("summary should contain file names but not full temp root paths.");
  }

  const auto formula_input = input_dir / "=cmd.webp";
  if (auto ok = write_webp(formula_input, std::byte{64}); !ok) {
    return fail(ok.error());
  }
  awj::ImageFile image{.index = 0,
                       .path = formula_input,
                       .bytes = std::filesystem::file_size(formula_input, ec)};
  awj::EncodeResult formula_result{.index = 0,
                                   .input_path = image.path,
                                   .output_path = output_dir / "=cmd.webp",
                                   .original_bytes = image.bytes,
                                   .output_bytes = image.bytes,
                                   .processed = true,
                                   .ok = true,
                                   .message = "=FORMULA"};
  if (auto csv_ok = awj::write_csv(output_dir / "csv-formula", std::span<const awj::EncodeResult>{&formula_result, 1}); !csv_ok) {
    return fail(csv_ok.error());
  }
  const auto formula_csv = read_text(output_dir / "csv-formula" / "summary.csv");
  if (formula_csv.find("'=cmd.webp") == std::string::npos ||
      formula_csv.find("'=FORMULA") == std::string::npos) {
    return fail("summary CSV did not escape formula-like cells.");
  }

  std::filesystem::remove_all(root, ec);
  return 0;
}
