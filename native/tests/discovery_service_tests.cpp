#include "discovery_service.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <atomic>
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

  std::atomic<int> errors = 0;
  std::uint64_t recovered = 0;
  feathercast::discovery_runtime::DiscoveryService resilient(
      [&](feathercast::app::DiscoveryResult result) {
        {
          std::lock_guard lock(mutex);
          recovered = result.generation;
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++errors;
        cv.notify_all();
      });
  resilient.Start([](const feathercast::app::DiscoveryRequest& request,
                     std::stop_token) {
    if (request.generation == 10) {
      throw std::runtime_error("discovery failure");
    }
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    return std::optional{std::move(result)};
  });
  feathercast::app::DiscoveryRequest failing;
  failing.generation = 10;
  assert(resilient.Refresh(failing));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return errors == 1; }));
  }
  feathercast::app::DiscoveryRequest recovery;
  recovery.generation = 11;
  assert(resilient.Refresh(recovery));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return recovered == 11; }));
  }
  resilient.Stop();
  return 0;
}
