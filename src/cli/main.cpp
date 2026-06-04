#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <print>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

import awj.config;
import awj.core;
import awj.pipeline;

namespace {

std::stop_source g_cli_stop_source;
std::atomic_bool g_cli_stop_requested{false};

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

int run_cli(int argc, wchar_t* argv[]) {
  // Windows 控制台默认代码页不是 UTF-8，先切换再输出中文日志。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
    print_line("[WARN] Ctrl+C 取消处理注册失败，继续运行。");
  }

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
    std::jthread studio_cancel_watcher;
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
    const auto exit_code = capture_expected<int>(
        [&] { return awj::run_pipeline(parsed->config, g_cli_stop_source.get_token()); });
    studio_cancel_watcher.request_stop();
    studio_cancel_watcher = {};
    studio_cancel_event.reset();
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
