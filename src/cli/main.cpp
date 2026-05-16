#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

import avif.config;
import avif.core;
import avif.pipeline;

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

template <class Value, class Function>
std::expected<Value, std::string> capture_expected(Function&& fn) noexcept {
  try {
    return std::invoke(std::forward<Function>(fn));
  } catch (const std::exception& ex) {
    return std::unexpected{std::string{ex.what()}};
  } catch (...) {
    return std::unexpected{std::string{"未知异常"}};
  }
}

int run_cli(int argc, wchar_t* argv[]) {
  // Windows 控制台默认代码页不是 UTF-8，先切换再输出中文日志。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  try {
    std::vector<std::wstring> args;
    args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    if (args.empty()) {
      avif::print_help();
      return 0;
    }

    const auto parsed = avif::parse_arguments(args);
    if (!parsed) {
      print_line(std::format("[FAIL] {}", parsed.error()));
      return 1;
    }
    if (parsed->should_exit) {
      return parsed->exit_code;
    }
    const auto exit_code =
        capture_expected<int>([&] { return avif::run_pipeline(parsed->config); });
    if (!exit_code) {
      print_line(std::format("[FAIL] {}", exit_code.error()));
      return 1;
    }
    return *exit_code;
  } catch (const std::exception& ex) {
    print_line(std::format("[FAIL] {}", ex.what()));
    return 1;
  } catch (...) {
    print_line("[FAIL] 未知异常，程序已安全退出。");
    return 1;
  }
}

int wmain(int argc, wchar_t* argv[]) {
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  // 静态 ImageMagick/AVIF/WebP delegate 组合偶尔会在进程静态析构阶段
  // 触发第三方清理代码崩溃。CLI 到这里时工作已完成，直接交还退出码。
  ExitProcess(static_cast<UINT>(exit_code));
}
