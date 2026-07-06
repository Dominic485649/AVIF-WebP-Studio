#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

int run_cli(int argc, wchar_t* argv[]);
int run_studio_ui();
int run_shell_convert_window(int argc, wchar_t* argv[]);

namespace {

void enable_process_dpi_awareness() noexcept {
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return;
  }
  using SetDpiAwarenessContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  auto set_dpi_awareness_context = reinterpret_cast<SetDpiAwarenessContext>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (set_dpi_awareness_context != nullptr) {
    set_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
}

void release_explorer_console_if_lonely() {
  std::array<DWORD, 2> process_ids{};
  const DWORD count = GetConsoleProcessList(
      process_ids.data(), static_cast<DWORD>(process_ids.size()));
  if (count <= 1) {
    FreeConsole();
  }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  enable_process_dpi_awareness();
  if (argc <= 1) {
    release_explorer_console_if_lonely();
    return run_studio_ui();
  }

  for (int i = 1; i < argc; ++i) {
    if (std::wcscmp(argv[i], L"--shell-window") == 0) {
      release_explorer_console_if_lonely();
      return run_shell_convert_window(argc, argv);
    }
  }

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
