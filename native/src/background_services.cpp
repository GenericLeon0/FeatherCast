#include "background_services.hpp"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace feathercast::runtime {

std::optional<DecodedIcon> DecodePngIcon(
    IWICImagingFactory* factory, const std::filesystem::path& path,
    const std::wstring& key) {
  if (!factory) return std::nullopt;
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
          decoder.GetAddressOf()))) {
    return std::nullopt;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return std::nullopt;
  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(
          frame.Get(), GUID_WICPixelFormat32bppPBGRA,
          WICBitmapDitherTypeNone, nullptr, 0,
          WICBitmapPaletteTypeMedianCut))) {
    return std::nullopt;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
      height == 0 || width > 512 || height > 512) {
    return std::nullopt;
  }
  DecodedIcon icon;
  icon.key = key;
  icon.width = width;
  icon.height = height;
  icon.stride = width * 4u;
  icon.pixels.resize(static_cast<std::size_t>(icon.stride) * height);
  if (FAILED(converter->CopyPixels(
          nullptr, icon.stride, static_cast<UINT>(icon.pixels.size()),
          icon.pixels.data()))) {
    return std::nullopt;
  }
  return icon;
}

void LaunchService::Start(std::size_t workers, ErrorHandler errorHandler) {
  executor_.Start(workers, std::move(errorHandler));
}

void LaunchService::Stop() {
  executor_.Shutdown();
}

bool LaunchService::Submit(Task task) {
  return executor_.Submit(std::move(task));
}

IconResolver::IconResolver(Completed completed, Failed failed)
    : completed_(std::move(completed)), failed_(std::move(failed)) {}

IconResolver::~IconResolver() {
  Stop();
}

void IconResolver::Start(std::size_t workers, Resolve resolve) {
  std::lock_guard lock(mutex_);
  if (!workers_.empty()) return;
  stopping_ = false;
  resolve_ = std::move(resolve);
  workers = std::max<std::size_t>(1, workers);
  workers_.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    workers_.emplace_back(
        [this](std::stop_token token) { WorkerLoop(token); });
  }
}

void IconResolver::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (workers_.empty()) return;
    stopping_ = true;
    jobs_.clear();
    pending_.clear();
    for (auto& worker : workers_) worker.request_stop();
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  std::lock_guard lock(mutex_);
  workers_.clear();
  resolve_ = {};
  stopping_ = false;
}

bool IconResolver::Queue(std::wstring key) {
  if (key.empty()) return false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || workers_.empty() || !resolve_) return false;
    if (!pending_.insert(key).second) return true;
    jobs_.push_back(std::move(key));
  }
  cv_.notify_one();
  return true;
}

void IconResolver::ClearPending() {
  std::lock_guard lock(mutex_);
  jobs_.clear();
  pending_.clear();
}

void IconResolver::WorkerLoop(std::stop_token stopToken) {
  const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(coResult)) {
    if (failed_) {
      try {
        failed_(std::make_exception_ptr(
            std::runtime_error("icon worker COM initialization failed")));
      } catch (...) {}
    }
    return;
  }
  for (;;) {
    std::wstring key;
    Resolve resolve;
    std::optional<DecodedIcon> decoded;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return !jobs_.empty() || stopping_ || stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) break;
      key = std::move(jobs_.front());
      jobs_.pop_front();
      resolve = resolve_;
    }
    try {
      if (!stopToken.stop_requested()) decoded = resolve(key, stopToken);
    } catch (...) {
      if (failed_) {
        try { failed_(std::current_exception()); } catch (...) {}
      }
    }
    {
      std::lock_guard lock(mutex_);
      pending_.erase(key);
    }
    if (!stopToken.stop_requested() && decoded && completed_) {
      try { completed_(std::move(*decoded)); } catch (...) {
        if (failed_) {
          try { failed_(std::current_exception()); } catch (...) {}
        }
      }
    }
  }
  CoUninitialize();
}

}  // namespace feathercast::runtime
