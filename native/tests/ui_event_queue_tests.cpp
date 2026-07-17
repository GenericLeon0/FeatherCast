#include "runtime_events.hpp"
#include "test_framework.hpp"
#include "ui_event_queue.hpp"

#include <atomic>
#include <string>
#include <variant>

int main() {
  using feathercast::runtime::BackgroundSubsystem;
  using feathercast::runtime::BackgroundTaskFailed;
  using feathercast::runtime::IconResolved;
  using feathercast::runtime::UiEvent;
  using feathercast::runtime::UiEventQueue;

  std::atomic<int> notifications = 0;
  UiEventQueue<UiEvent> queue([&] { ++notifications; });

  assert(queue.Push(IconResolved{L"one"}));
  assert(queue.Push(IconResolved{L"two"}));
  assert(notifications.load() == 1);

  auto events = queue.Drain();
  assert(events.size() == 2);
  assert(std::get<IconResolved>(events[0]).key == L"one");
  assert(std::get<IconResolved>(events[1]).key == L"two");
  assert(queue.Empty());

  assert(queue.Push(BackgroundTaskFailed{
      BackgroundSubsystem::Persistence, L"save failed"}));
  assert(notifications.load() == 2);
  events = queue.Drain();
  assert(events.size() == 1);
  assert(std::get<BackgroundTaskFailed>(events.front()).message ==
         L"save failed");

  queue.Close();
  assert(queue.Closed());
  assert(!queue.Push(IconResolved{L"ignored"}));
  assert(queue.Drain().empty());
  return 0;
}
