#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace feathercast::preview {

enum class Kind { Metadata, Text, Image, Error };

struct Request {
  std::uint64_t generation = 0;
  std::filesystem::path path;
  std::wstring terms;
};

struct Result {
  std::uint64_t generation = 0;
  Kind kind = Kind::Metadata;
  std::filesystem::path path;
  std::wstring title;
  std::wstring detail;
  std::wstring text;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::vector<std::byte> pixels;
};

class PreviewService {
 public:
  using ResultSink = std::function<void(Result)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit PreviewService(ResultSink sink = {}, ErrorSink errors = {});
  ~PreviewService();
  PreviewService(const PreviewService&) = delete;
  PreviewService& operator=(const PreviewService&) = delete;

  void Start();
  void Stop();
  bool Load(Request request);
  void Invalidate(std::uint64_t generation);

 private:
  void WorkerLoop(std::stop_token token);
  Result Build(const Request& request, std::stop_token token) const;

  ResultSink sink_;
  ErrorSink errors_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<Request> pending_;
  std::atomic<std::uint64_t> generation_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::preview
