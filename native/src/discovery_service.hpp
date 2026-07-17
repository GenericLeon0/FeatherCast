#pragma once

#include "app_types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace feathercast::discovery_runtime {

class DiscoveryService {
 public:
  using Worker = std::function<std::optional<app::DiscoveryResult>(
      const app::DiscoveryRequest&, std::stop_token)>;
  using ResultSink = std::function<void(app::DiscoveryResult)>;

  explicit DiscoveryService(ResultSink resultSink = {});
  ~DiscoveryService();

  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;

  void Start(Worker worker);
  void Stop();
  bool Refresh(app::DiscoveryRequest request);
  std::uint64_t Cancel();
  bool IsCurrent(std::uint64_t generation) const;

 private:
  void WorkerLoop(std::stop_token stopToken);

  ResultSink resultSink_;
  Worker workerFunction_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<app::DiscoveryRequest> pending_;
  std::atomic<std::uint64_t> latestGeneration_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::discovery_runtime
