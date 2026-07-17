#include "discovery_service.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

int main() {
  std::mutex mutex;
  std::condition_variable cv;
  std::uint64_t published = 0;

  feathercast::discovery_runtime::DiscoveryService service(
      [&](feathercast::app::DiscoveryResult result) {
        {
          std::lock_guard lock(mutex);
          published = result.generation;
        }
        cv.notify_all();
      });
  service.Start([](const feathercast::app::DiscoveryRequest& request,
                   std::stop_token) {
    if (request.generation == 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    return std::optional{std::move(result)};
  });

  feathercast::app::DiscoveryRequest first;
  first.generation = 1;
  assert(service.Refresh(first));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  feathercast::app::DiscoveryRequest second;
  second.generation = 2;
  assert(service.Refresh(second));

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return published == 2; }));
  }
  assert(service.IsCurrent(2));
  assert(service.Cancel() == 3);
  assert(!service.IsCurrent(2));
  service.Stop();
  assert(!service.Refresh(second));
  return 0;
}
