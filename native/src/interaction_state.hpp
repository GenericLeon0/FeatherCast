#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace feathercast::interaction {

class SearchPresentationState {
 public:
  std::uint64_t NextRequest() { return ++requested_; }

  void Invalidate() { ++requested_; }

  void MarkCompleted(std::uint64_t generation) {
    completed_ = std::max(completed_, generation);
  }

  bool Publish(std::uint64_t generation) {
    MarkCompleted(generation);
    if (generation != requested_) return false;
    displayed_ = generation;
    return true;
  }

  bool Pending() const { return displayed_ != requested_; }

  bool CanActivate() const {
    return !Pending() && displayed_ == completed_;
  }

  std::uint64_t Requested() const { return requested_; }
  std::uint64_t Displayed() const { return displayed_; }
  std::uint64_t Completed() const { return completed_; }

 private:
  std::uint64_t requested_ = 0;
  std::uint64_t displayed_ = 0;
  std::uint64_t completed_ = 0;
};

inline bool StablePointerTargetMatches(
    int expectedType, int expectedIndex, std::uint64_t expectedGeneration,
    std::wstring_view expectedKey, int currentType, int currentIndex,
    std::uint64_t currentGeneration, std::wstring_view currentKey,
    bool pointerInside, bool activationAllowed) {
  return pointerInside && activationAllowed && expectedType == currentType &&
         expectedIndex == currentIndex &&
         expectedGeneration == currentGeneration && expectedKey == currentKey;
}

}  // namespace feathercast::interaction
