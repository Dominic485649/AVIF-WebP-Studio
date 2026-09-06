#ifdef _WIN32

#include "../src/ui/file_drop_win32.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj_core.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

class HdropDataObject final : public IDataObject {
 public:
  explicit HdropDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *object = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return refs_.fetch_sub(1, std::memory_order_relaxed) - 1;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (format == nullptr || medium == nullptr) return E_POINTER;
    if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;

    std::size_t wchar_count = 1;
    for (const auto& path : paths_) wchar_count += path.size() + 1;
    const std::size_t bytes = sizeof(DROPFILES) + wchar_count * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (storage == nullptr) return E_OUTOFMEMORY;
    auto* base = static_cast<std::byte*>(GlobalLock(storage));
    if (base == nullptr) {
      GlobalFree(storage);
      return E_OUTOFMEMORY;
    }

    auto* header = reinterpret_cast<DROPFILES*>(base);
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(base + sizeof(DROPFILES));
    for (const auto& path : paths_) {
      std::memcpy(cursor, path.c_str(), path.size() * sizeof(wchar_t));
      cursor += path.size();
      *cursor++ = L'\0';
    }
    *cursor = L'\0';
    GlobalUnlock(storage);

    std::memset(medium, 0, sizeof(*medium));
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = storage;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format == nullptr) return E_POINTER;
    return format->cfFormat == CF_HDROP && format->dwAspect == DVASPECT_CONTENT &&
                   format->tymed == TYMED_HGLOBAL
               ? S_OK
               : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
    if (output != nullptr) output->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  std::atomic<ULONG> refs_{1};
  std::vector<std::wstring> paths_;
};

class StaticDropTarget final : public IDropTarget {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *object = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return refs_.fetch_sub(1, std::memory_order_relaxed) - 1;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD*) override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
};

}  // namespace

int main() {
  using awj::ui_drop::InstallRetryAction;
  if (awj::ui_drop::install_retry_action(false, 0, 3) != InstallRetryAction::retry ||
      awj::ui_drop::install_retry_action(false, 1, 3) != InstallRetryAction::retry ||
      awj::ui_drop::install_retry_action(false, 2, 3) != InstallRetryAction::exhausted ||
      awj::ui_drop::install_retry_action(true, 0, 3) != InstallRetryAction::install ||
      awj::ui_drop::install_retry_action(true, 2, 3) != InstallRetryAction::install ||
      awj::ui_drop::install_retry_action(false, 0, 0) != InstallRetryAction::exhausted) {
    return fail("native drop install retry policy mismatch");
  }

  if (awj::ui_drop::install(nullptr, {})) {
    return fail("invalid HWND was accepted for native drop registration");
  }

  std::size_t retry_attempts = 0;
  std::size_t install_signals = 0;
  bool install_finished = false;
  for (const bool hwnd_ready : {false, false, true, true}) {
    if (install_finished) continue;
    const auto action = awj::ui_drop::install_retry_action(
        hwnd_ready, retry_attempts, 4);
    ++retry_attempts;
    if (action == InstallRetryAction::install) {
      ++install_signals;
      install_finished = true;
    } else if (action == InstallRetryAction::exhausted) {
      install_finished = true;
    }
  }
  if (install_signals != 1 || !install_finished) {
    return fail("native drop retry orchestration did not install exactly once");
  }

  struct EffectCase {
    DWORD allowed{};
    DWORD expected_effect{};
    std::size_t expected_callbacks{};
  };
  constexpr EffectCase effect_cases[] = {
      {DROPEFFECT_COPY, DROPEFFECT_COPY, 1},
      {DROPEFFECT_MOVE, DROPEFFECT_NONE, 0},
      {DROPEFFECT_LINK, DROPEFFECT_NONE, 0},
      {DROPEFFECT_NONE, DROPEFFECT_NONE, 0},
      {DROPEFFECT_COPY | DROPEFFECT_MOVE, DROPEFFECT_COPY, 1},
      {DROPEFFECT_COPY | DROPEFFECT_LINK, DROPEFFECT_COPY, 1},
  };
  for (const auto& test : effect_cases) {
    // DragEnter and DragOver both consume this exact shared decision. Keep the
    // OLE effect and visible hover validity locked together for every mask.
    const auto feedback = awj::ui_drop::copy_drag_feedback(test.allowed, true, 1);
    const bool expected_valid = test.expected_effect == DROPEFFECT_COPY;
    if (feedback.effect != test.expected_effect || !feedback.hover.active ||
        feedback.hover.valid != expected_valid || feedback.hover.item_count != 1) {
      return fail("DragEnter/DragOver hover/effect contract mismatch");
    }

    std::size_t callback_count = 0;
    const DWORD result = awj::ui_drop::dispatch_copy_drop(
        test.allowed, true, [&] {
          ++callback_count;
          return true;
        });
    if (result != test.expected_effect || callback_count != test.expected_callbacks) {
      return fail("COPY-only effect contract mismatch");
    }
  }

  const auto cannot_accept_feedback =
      awj::ui_drop::copy_drag_feedback(DROPEFFECT_COPY, false, 1);
  if (cannot_accept_feedback.effect != DROPEFFECT_NONE ||
      cannot_accept_feedback.hover.valid || !cannot_accept_feedback.hover.active ||
      cannot_accept_feedback.hover.item_count != 1) {
    return fail("can_accept=false advertised a valid drag hover");
  }
  const auto empty_feedback =
      awj::ui_drop::copy_drag_feedback(DROPEFFECT_COPY, true, 0);
  if (empty_feedback.effect != DROPEFFECT_NONE || empty_feedback.hover.valid ||
      !empty_feedback.hover.active || empty_feedback.hover.item_count != 0) {
    return fail("empty CF_HDROP advertised a valid drag hover");
  }

  std::size_t callback_count = 0;
  if (awj::ui_drop::dispatch_copy_drop(DROPEFFECT_COPY, false, [&] {
        ++callback_count;
        return true;
      }) != DROPEFFECT_NONE ||
      callback_count != 0) {
    return fail("can_accept=false invoked drop callback");
  }
  if (awj::ui_drop::dispatch_copy_drop(DROPEFFECT_COPY, true, [&] {
        ++callback_count;
        return false;
      }) != DROPEFFECT_NONE ||
      callback_count != 1) {
    return fail("failed consumer did not return no-effect");
  }
  if (awj::ui_drop::dispatch_copy_drop(DROPEFFECT_COPY, true, [&]() -> bool {
        ++callback_count;
        throw std::runtime_error("consumer failure");
      }) != DROPEFFECT_NONE ||
      callback_count != 2) {
    return fail("throwing consumer escaped fail-closed effect contract");
  }
  if (awj::ui_drop::dispatch_copy_drop(
          DROPEFFECT_COPY, true, std::function<bool()>{}) != DROPEFFECT_NONE) {
    return fail("missing consumer was accepted");
  }

  const std::wstring long_path = L"C:\\" + std::wstring(280, L'x') + L".png";
  const std::vector<std::wstring> expected{
      L"C:\\Images\\a.png", L"C:\\图片\\测试 文件.avif", long_path};
  HdropDataObject data{expected};

  const auto count = awj::ui_drop::hdrop_item_count(&data);
  if (!count || *count != expected.size()) return fail("CF_HDROP count mismatch");

  const auto paths = awj::ui_drop::extract_hdrop_paths(&data);
  if (!paths || paths->size() != expected.size()) return fail("CF_HDROP extraction failed");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if ((*paths)[index].native() != expected[index]) return fail("CF_HDROP path mismatch");
  }

  const HRESULT ole_hr = OleInitialize(nullptr);
  if (FAILED(ole_hr)) return fail("OleInitialize failed for registration test");
  const wchar_t* class_name = L"AWJ.FileDropWin32Tests";
  WNDCLASSW wc{};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = class_name;
  if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    OleUninitialize();
    return fail("RegisterClassW failed");
  }
  HWND hwnd = CreateWindowExW(0, class_name, L"", WS_OVERLAPPED,
                              0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
  if (hwnd == nullptr) {
    OleUninitialize();
    return fail("CreateWindowExW failed");
  }

  StaticDropTarget original;
  StaticDropTarget probe;
  if (RegisterDragDrop(hwnd, &original) != S_OK) {
    DestroyWindow(hwnd);
    UnregisterClassW(class_name, wc.hInstance);
    OleUninitialize();
    return fail("failed to seed existing drop target");
  }
  {
    auto registration = awj::ui_drop::install(hwnd, {});
    if (!registration || !registration->active()) return fail("ownership transfer install failed");
    if (RegisterDragDrop(hwnd, &probe) != DRAGDROP_E_ALREADYREGISTERED) {
      return fail("AWJ drop target was not registered after ownership transfer");
    }
  }
  if (RegisterDragDrop(hwnd, &probe) != S_OK) {
    return fail("AWJ Registration destructor did not revoke its target");
  }
  RevokeDragDrop(hwnd);
  DestroyWindow(hwnd);
  UnregisterClassW(class_name, wc.hInstance);
  OleUninitialize();

  std::cout << "file drop win32 tests passed\n";
  return 0;
}

#endif
