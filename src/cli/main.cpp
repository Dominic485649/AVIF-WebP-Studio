#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <csignal>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

import awj.config;
import awj.core;
import awj.pipeline;
import awj.preset;

namespace {

std::stop_source g_cli_stop_source;
std::atomic_bool g_cli_stop_requested{false};
#ifndef _WIN32
volatile std::sig_atomic_t g_cli_signal_seen = 0;
#endif

#ifdef _WIN32
struct Win32HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueWin32Handle = std::unique_ptr<void, Win32HandleDeleter>;

BOOL WINAPI console_control_handler(DWORD event) {
  switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      if (g_cli_stop_requested.exchange(true)) {
        return FALSE;
      }
      g_cli_stop_source.request_stop();
      return TRUE;
    default:
      return FALSE;
  }
}

#else

void signal_handler(int) {
  g_cli_signal_seen = 1;
}

#endif

}  // namespace

void print_line(std::string_view text) {
  std::println("{}", text);
}

template <class Value, class Function>
std::expected<Value, std::string> capture_expected(Function&& fn) noexcept {
  try {
    return std::invoke(std::forward<Function>(fn));
  } catch (const std::exception&) {
    return std::unexpected{std::string{}};
  } catch (...) {
    return std::unexpected{std::string{}};
  }
}

int run_cli_args(std::vector<std::wstring> args) {
  g_cli_stop_source = std::stop_source{};
  g_cli_stop_requested.store(false, std::memory_order_release);
#ifndef _WIN32
  g_cli_signal_seen = 0;
#endif
#ifdef _WIN32
  // Windows 控制台默认代码页不是 UTF-8，先切换再输出中文日志。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
    print_line("[WARN] Ctrl+C 取消处理注册失败，继续运行。");
  }
#else
  std::signal(SIGINT, signal_handler);
  std::jthread signal_watcher{[](std::stop_token token) {
    while (!token.stop_requested()) {
      if (g_cli_signal_seen != 0) {
        if (!g_cli_stop_requested.exchange(true)) {
          g_cli_stop_source.request_stop();
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }};
#endif

  try {
    if (args.empty()) {
      awj::print_help();
      return 0;
    }

    const auto parsed = awj::parse_arguments_with_user_preset(args);
    if (!parsed) {
      print_line(std::format("[FAIL] {}", parsed.error()));
      return 1;
    }
    for (const auto& warning : parsed->warnings) {
      print_line(std::format("[WARN] {}", warning));
    }
    if (parsed->should_exit) {
      return parsed->exit_code;
    }
    if (auto valid = awj::validate_execution_config(parsed->config); !valid) {
      print_line(std::format("[FAIL] {}", valid.error()));
      return 1;
    }
    std::jthread studio_cancel_watcher;
#ifdef _WIN32
    UniqueWin32Handle studio_cancel_event;
    if (!parsed->config.studio_cancel_event_name.empty()) {
      studio_cancel_event.reset(OpenEventW(SYNCHRONIZE, FALSE,
                                           parsed->config.studio_cancel_event_name.c_str()));
      if (studio_cancel_event != nullptr) {
        studio_cancel_watcher = std::jthread{[](std::stop_token token, HANDLE event_handle) {
          while (!token.stop_requested()) {
            const DWORD wait = WaitForSingleObject(event_handle, 100);
            if (wait == WAIT_OBJECT_0) {
              g_cli_stop_requested.store(true, std::memory_order_release);
              g_cli_stop_source.request_stop();
              return;
            }
            if (wait != WAIT_TIMEOUT) {
              return;
            }
          }
        }, studio_cancel_event.get()};
      }
    }
#endif
    const auto exit_code = capture_expected<int>([&] {
      const auto token = g_cli_stop_source.get_token();
      if (parsed->config.output_policy == awj::OutputPolicy::shell &&
          !parsed->shell_inputs.empty()) {
        return awj::run_pipeline(parsed->config, parsed->shell_inputs, token);
      }
      return awj::run_pipeline(parsed->config, token);
    });
    studio_cancel_watcher.request_stop();
    studio_cancel_watcher = {};
#ifdef _WIN32
    studio_cancel_event.reset();
#endif
    if (!exit_code) {
      if (exit_code.error().empty()) {
        print_line("[FAIL] 运行时异常，程序已安全退出。");
      } else {
        print_line(std::format("[FAIL] {}", exit_code.error()));
      }
      return 1;
    }
    return *exit_code;
  } catch (const std::exception&) {
    print_line("[FAIL] 运行时异常，程序已安全退出。");
    return 1;
  } catch (...) {
    print_line("[FAIL] 未知异常，程序已安全退出。");
    return 1;
  }
}

#ifdef _WIN32
int run_cli(int argc, wchar_t* argv[]) {
  std::vector<std::wstring> args;
  args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return run_cli_args(std::move(args));
}
#else
int run_cli(int argc, char* argv[]) {
  std::vector<std::wstring> args;
  args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(awj::wide_from_utf8(argv[i] != nullptr ? std::string_view{argv[i]} : std::string_view{}));
  }
  return run_cli_args(std::move(args));
}
#endif

#ifndef AWJ_UNIFIED_EXE
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  // 部分第三方 native codec 的静态运行时会在进程静态析构阶段
  // 触发清理顺序问题。CLI 到这里时工作已完成，直接交还退出码；
  // std::_Exit 避免运行 CRT/第三方静态析构，比 ExitProcess 更稳定。
  std::_Exit(exit_code);
}
#else
int main(int argc, char* argv[]) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  return exit_code;
}
#endif
#endif
