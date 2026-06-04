#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>

int run_cli(int argc, wchar_t* argv[]);
int run_studio_ui();

namespace {

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
  if (argc <= 1) {
    release_explorer_console_if_lonely();
    return run_studio_ui();
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
