#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

import awj.jxr_codec;

namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

struct ComInit {
  HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)};

  ~ComInit() {
    if (SUCCEEDED(hr)) {
      CoUninitialize();
    }
  }
};

bool write_test_jxr(const std::filesystem::path& path) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr)) {
    return false;
  }
  hr = stream->InitializeFromFilename(path.native().c_str(), GENERIC_WRITE);
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder);
  if (FAILED(hr)) {
    return false;
  }
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  hr = encoder->CreateNewFrame(&frame, &options);
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->Initialize(options.Get());
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->SetSize(2, 2);
  if (FAILED(hr)) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr) || !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
    return false;
  }
  std::uint8_t pixels[] = {
      0, 0, 255, 0, 255, 0,
      255, 0, 0, 255, 255, 255,
  };
  hr = frame->WritePixels(2, 6, static_cast<UINT>(sizeof(pixels)), pixels);
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->Commit();
  if (FAILED(hr)) {
    return false;
  }
  hr = encoder->Commit();
  return SUCCEEDED(hr);
}

int decode_and_check(const std::filesystem::path& path) {
  awj::JxrImageDecoder decoder;
  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions || dimensions->width != 2 || dimensions->height != 2) {
    return fail(dimensions ? "JXR dimensions invalid." : dimensions.error());
  }
  auto decoded = decoder.decode(path);
  if (!decoded || decoded->decoder_id != "windows-jxr" || decoded->used_fallback ||
      decoded->image.width != 2 || decoded->image.height != 2 ||
      decoded->image.planes.empty()) {
    return fail(decoded ? "JXR decode result invalid." : decoded.error());
  }
  return 0;
}

}  // namespace

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  ComInit init;
  if (FAILED(init.hr) && init.hr != RPC_E_CHANGED_MODE) {
    return fail("COM initialization failed for JXR test.");
  }

  const auto temp = std::filesystem::temp_directory_path();
  const auto jxr = temp / "awj-jxr-codec-test.jxr";
  const auto wdp = temp / "awj-jxr-codec-test.wdp";
  const auto hdp = temp / "awj-jxr-codec-test.hdp";
  std::error_code ec;
  std::filesystem::remove(jxr, ec);
  std::filesystem::remove(wdp, ec);
  std::filesystem::remove(hdp, ec);

  if (!write_test_jxr(jxr)) {
    return fail("Could not create temporary JPEG XR test input.");
  }
  std::filesystem::copy_file(jxr, wdp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::filesystem::remove(jxr, ec);
    return fail("Could not copy temporary WDP test input.");
  }
  std::filesystem::copy_file(jxr, hdp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::filesystem::remove(jxr, ec);
    std::filesystem::remove(wdp, ec);
    return fail("Could not copy temporary HDP test input.");
  }

  if (const auto code = decode_and_check(jxr); code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(wdp); code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(hdp); code != 0) {
    return code;
  }

  std::filesystem::remove(jxr, ec);
  std::filesystem::remove(wdp, ec);
  std::filesystem::remove(hdp, ec);
  return 0;
}
