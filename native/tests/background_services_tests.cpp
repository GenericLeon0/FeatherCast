#include "background_services.hpp"
#include "test_framework.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>

int main() {
  const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  assert(SUCCEEDED(coResult));
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    assert(SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))));
    const auto iconPath = std::filesystem::temp_directory_path() /
                          (L"feathercast-icon-decode-" +
                           std::to_wstring(GetCurrentProcessId()) + L".png");
    std::error_code ec;
    std::filesystem::remove(iconPath, ec);
    Microsoft::WRL::ComPtr<IWICStream> stream;
    assert(SUCCEEDED(factory->CreateStream(&stream)));
    assert(SUCCEEDED(
        stream->InitializeFromFilename(iconPath.c_str(), GENERIC_WRITE)));
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    assert(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                            &encoder)));
    assert(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    assert(SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)));
    assert(SUCCEEDED(frame->Initialize(properties.Get())));
    assert(SUCCEEDED(frame->SetSize(2, 1)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    assert(SUCCEEDED(frame->SetPixelFormat(&format)));
    const std::uint8_t pixels[] = {0, 0, 255, 255, 0, 255, 0, 255};
    assert(SUCCEEDED(frame->WritePixels(1, 8, sizeof(pixels),
                                        const_cast<BYTE*>(pixels))));
    assert(SUCCEEDED(frame->Commit()));
    assert(SUCCEEDED(encoder->Commit()));
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    const auto decoded = feathercast::runtime::DecodePngIcon(
        factory.Get(), iconPath, L"test-icon");
    assert(decoded && decoded->key == L"test-icon");
    assert(decoded->width == 2 && decoded->height == 1 &&
           decoded->stride == 8 && decoded->pixels.size() == 8);
    assert(decoded->pixels[2] == 255 && decoded->pixels[3] == 255);
    std::filesystem::remove(iconPath, ec);
  }

  feathercast::runtime::LaunchService launch;
  launch.Start(1);
  std::promise<void> ran;
  auto future = ran.get_future();
  assert(launch.Submit([&](std::stop_token) { ran.set_value(); }));
  assert(future.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  launch.Stop();

  std::mutex mutex;
  std::condition_variable cv;
  int resolved = 0;
  int completed = 0;
  feathercast::runtime::IconResolver icons(
      [&](feathercast::runtime::DecodedIcon) {
    {
      std::lock_guard lock(mutex);
      ++completed;
    }
    cv.notify_all();
      });
  icons.Start(2, [&](const std::wstring& key, std::stop_token) {
    ++resolved;
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });
  assert(icons.Queue(L"same"));
  assert(icons.Queue(L"same"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return completed == 1; }));
  }
  assert(resolved == 1);
  icons.Stop();
  assert(!icons.Queue(L"stopped"));

  std::atomic<int> iconErrors = 0;
  bool iconRecovered = false;
  feathercast::runtime::IconResolver resilientIcons(
      [&](feathercast::runtime::DecodedIcon icon) {
        {
          std::lock_guard lock(mutex);
          iconRecovered = icon.key == L"good";
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++iconErrors;
        cv.notify_all();
      });
  resilientIcons.Start(1, [](const std::wstring& key, std::stop_token) {
    if (key == L"bad") throw std::runtime_error("icon failure");
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });
  assert(resilientIcons.Queue(L"bad"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return iconErrors == 1; }));
  }
  assert(resilientIcons.Queue(L"good"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return iconRecovered; }));
  }
  resilientIcons.Stop();

  std::promise<int> operationCompleted;
  auto operationFuture = operationCompleted.get_future();
  feathercast::runtime::SingleOperationService<int> operation(
      [&](int value) { operationCompleted.set_value(value); });
  assert(operation.Run([](std::stop_token) -> std::optional<int> {
    return 42;
  }));
  assert(operationFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  assert(operationFuture.get() == 42);
  operation.Stop();

  std::promise<void> operationFailed;
  auto failedFuture = operationFailed.get_future();
  feathercast::runtime::SingleOperationService<int> failingOperation(
      [](int) {}, [&](std::exception_ptr) { operationFailed.set_value(); });
  assert(failingOperation.Run([](std::stop_token) -> std::optional<int> {
    throw std::runtime_error("operation failure");
  }));
  assert(failedFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  failingOperation.Stop();

  std::promise<void> canceled;
  auto canceledFuture = canceled.get_future();
  feathercast::runtime::SingleOperationService<int> cancelable(
      [](int) {});
  assert(cancelable.Run([&](std::stop_token token) -> std::optional<int> {
    while (!token.stop_requested()) std::this_thread::yield();
    canceled.set_value();
    return std::nullopt;
  }));
  cancelable.Stop();
  assert(canceledFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  CoUninitialize();
  return 0;
}
