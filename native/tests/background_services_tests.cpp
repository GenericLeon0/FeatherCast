#include "background_services.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

int main() {
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
  feathercast::runtime::IconResolver icons([&](std::wstring) {
    {
      std::lock_guard lock(mutex);
      ++completed;
    }
    cv.notify_all();
  });
  icons.Start(2, [&](const std::wstring&, std::stop_token) {
    ++resolved;
    return true;
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
  return 0;
}
