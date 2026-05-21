#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

import awj.config;

namespace {

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

int run_config_cli(int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  try {
    std::vector<std::wstring> args;
    args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    if (args.empty()) {
      awj::print_help();
      return 0;
    }

    const auto parsed = awj::parse_arguments(args);
    if (!parsed) {
      print_line(std::format("[FAIL] {}", parsed.error()));
      return 1;
    }
    if (parsed->should_exit) {
      return parsed->exit_code;
    }

    print_line("[OK] config parsed.");
    return 0;
  } catch (const std::exception& ex) {
    print_line(std::format("[FAIL] {}", ex.what()));
    return 1;
  } catch (...) {
    print_line("[FAIL] 未知异常，程序已安全退出。");
    return 1;
  }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  const int exit_code = run_config_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}
