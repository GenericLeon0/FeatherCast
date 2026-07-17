#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace feathercast::runtime {

template <typename Event>
class UiEventQueue {
 public:
  using Notifier = std::function<void()>;

  explicit UiEventQueue(Notifier notifier = {})
      : notifier_(std::move(notifier)) {}

  UiEventQueue(const UiEventQueue&) = delete;
  UiEventQueue& operator=(const UiEventQueue&) = delete;

  void SetNotifier(Notifier notifier) {
    std::lock_guard lock(mutex_);
    notifier_ = std::move(notifier);
  }

  bool Push(Event event) {
    Notifier notifier;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return false;
      events_.push_back(std::move(event));
      if (notificationPending_) return true;
      notificationPending_ = true;
      notifier = notifier_;
    }
    if (notifier) notifier();
    return true;
  }

  std::vector<Event> Drain() {
    std::vector<Event> drained;
    {
      std::lock_guard lock(mutex_);
      drained.reserve(events_.size());
      while (!events_.empty()) {
        drained.push_back(std::move(events_.front()));
        events_.pop_front();
      }
      notificationPending_ = false;
    }
    return drained;
  }

  void Close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    events_.clear();
    notificationPending_ = false;
    notifier_ = {};
  }

  bool Closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  bool Empty() const {
    std::lock_guard lock(mutex_);
    return events_.empty();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Event> events_;
  Notifier notifier_;
  bool notificationPending_ = false;
  bool closed_ = false;
};

}  // namespace feathercast::runtime
