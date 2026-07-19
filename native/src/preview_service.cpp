#include "preview_service.hpp"

#include "core.hpp"
#include "file_content.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace feathercast::preview {
namespace {

constexpr std::uint64_t kMaxImageBytes = 25ULL * 1024 * 1024;
constexpr std::uint64_t kMaxImagePixels = 40ULL * 1000 * 1000;
constexpr std::uint32_t kMaxPreviewDimension = 1024;
constexpr std::size_t kMaxTextExcerptBytes = 16 * 1024;
constexpr std::size_t kMaxTextExcerptChars =
    kMaxTextExcerptBytes / sizeof(wchar_t);

std::wstring MetadataDetail(const std::filesystem::path& path,
                            std::uintmax_t bytes,
                            std::filesystem::file_time_type writeTime) {
  const auto ticks = std::chrono::duration_cast<std::chrono::seconds>(
                         writeTime.time_since_epoch())
                         .count();
  std::wostringstream out;
  out << path.wstring() << L"\n" << bytes << L" bytes\nModified " << ticks;
  return out.str();
}

std::wstring Excerpt(const std::wstring& text, const std::wstring& terms) {
  if (text.size() <= kMaxTextExcerptChars) return text;
  const std::wstring lowered = core::Lower(text);
  std::size_t match = std::wstring::npos;
  std::wstring token;
  for (const wchar_t ch : terms) {
    if (std::iswalnum(ch) || ch == L'_') token.push_back(ch);
    else if (!token.empty()) break;
  }
  if (!token.empty()) match = lowered.find(core::Lower(token));
  if (match == std::wstring::npos) return text.substr(0, kMaxTextExcerptChars);
  const std::size_t before = kMaxTextExcerptChars / 3;
  const std::size_t start = match > before ? match - before : 0;
  return text.substr(start, kMaxTextExcerptChars);
}

bool DecodeImage(const std::filesystem::path& path, Result& result,
                 std::stop_token token) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          &decoder))) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > kMaxImagePixels) {
    return false;
  }
  if (token.stop_requested()) return false;
  const double scale = std::min(
      1.0, static_cast<double>(kMaxPreviewDimension) /
               static_cast<double>(std::max(width, height)));
  const UINT targetWidth = std::max<UINT>(1, static_cast<UINT>(width * scale));
  const UINT targetHeight = std::max<UINT>(1, static_cast<UINT>(height * scale));
  ComPtr<IWICBitmapSource> source = frame;
  ComPtr<IWICBitmapScaler> scaler;
  if (targetWidth != width || targetHeight != height) {
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeFant))) {
      return false;
    }
    source = scaler;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  result.width = targetWidth;
  result.height = targetHeight;
  result.stride = targetWidth * 4;
  result.pixels.resize(static_cast<std::size_t>(result.stride) * targetHeight);
  return SUCCEEDED(converter->CopyPixels(
      nullptr, result.stride, static_cast<UINT>(result.pixels.size()),
      reinterpret_cast<BYTE*>(result.pixels.data())));
}

}  // namespace

PreviewService::PreviewService(ResultSink sink, ErrorSink errors)
    : sink_(std::move(sink)), errors_(std::move(errors)) {}

PreviewService::~PreviewService() { Stop(); }

void PreviewService::Start() {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  stopping_ = false;
  worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
}

void PreviewService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  stopping_ = false;
}

bool PreviewService::Load(Request request) {
  generation_.store(request.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable()) return false;
    pending_ = std::move(request);
  }
  cv_.notify_one();
  return true;
}

void PreviewService::Invalidate(std::uint64_t generation) {
  generation_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  pending_.reset();
}

void PreviewService::WorkerLoop(std::stop_token token) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (;;) {
    Request request;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return stopping_ || token.stop_requested() || pending_.has_value();
      });
      if (stopping_ || token.stop_requested()) break;
      request = std::move(*pending_);
      pending_.reset();
    }
    try {
      auto result = Build(request, token);
      if (!token.stop_requested() &&
          generation_.load(std::memory_order_acquire) == request.generation &&
          sink_) {
        sink_(std::move(result));
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
    }
  }
  CoUninitialize();
}

Result PreviewService::Build(const Request& request,
                             std::stop_token token) const {
  Result result;
  result.generation = request.generation;
  result.path = request.path;
  result.title = request.path.filename().wstring();
  std::error_code ec;
  const auto bytes = std::filesystem::is_directory(request.path, ec)
                         ? 0
                         : std::filesystem::file_size(request.path, ec);
  if (ec || !std::filesystem::exists(request.path, ec)) {
    result.kind = Kind::Error;
    result.detail = L"The selected file is no longer available.";
    return result;
  }
  const auto writeTime = std::filesystem::last_write_time(request.path, ec);
  result.detail = MetadataDetail(request.path, bytes, writeTime);
  if (std::filesystem::is_directory(request.path, ec)) return result;
  if (token.stop_requested()) return result;

  if (file_content::Supports(request.path)) {
    const auto extraction = file_content::Extract(
        request.path, file_content::kMaxIndexedBytes, token);
    if (extraction.state == file_content::State::Indexed) {
      result.kind = Kind::Text;
      result.text = Excerpt(extraction.text, request.terms);
    } else if (extraction.state != file_content::State::Unsupported) {
      result.detail += L"\nText preview is unavailable.";
    }
    return result;
  }
  if (file_content::IsImage(request.path) && bytes <= kMaxImageBytes) {
    if (DecodeImage(request.path, result, token)) {
      result.kind = Kind::Image;
    } else {
      result.kind = Kind::Error;
      result.detail += L"\nThe image could not be safely decoded.";
    }
  }
  return result;
}

}  // namespace feathercast::preview
