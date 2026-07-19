#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace feathercast::interaction {

enum class OverlayFocusPhase {
  Idle,
  Acquiring,
  Stabilizing,
  Closing,
};

class OverlayFocusSession {
 public:
  static constexpr std::uint64_t kGuardDurationMs = 300;

  std::uint64_t Begin(std::uint64_t now) {
    ++generation_;
    phase_ = OverlayFocusPhase::Acquiring;
    deadline_ = now + kGuardDurationMs;
    activationAttempts_ = 0;
    return generation_;
  }

  bool GuardActive(std::uint64_t now) const {
    return (phase_ == OverlayFocusPhase::Acquiring ||
            phase_ == OverlayFocusPhase::Stabilizing) &&
           now < deadline_;
  }

  bool AcceptsExternalCandidate(std::uint64_t now) const {
    return GuardActive(now);
  }

  void RecordActivationAttempt(bool activated) {
    ++activationAttempts_;
    if (activated && phase_ == OverlayFocusPhase::Acquiring) {
      phase_ = OverlayFocusPhase::Stabilizing;
    }
  }

  void Expire(std::uint64_t now) {
    if (!GuardActive(now) && phase_ != OverlayFocusPhase::Closing) {
      phase_ = OverlayFocusPhase::Idle;
    }
  }

  void BeginClosing() { phase_ = OverlayFocusPhase::Closing; }

  bool CanFinishClose(std::uint64_t generation) const {
    return phase_ == OverlayFocusPhase::Closing && generation == generation_;
  }

  void End() { phase_ = OverlayFocusPhase::Idle; }

  OverlayFocusPhase Phase() const { return phase_; }
  std::uint64_t Generation() const { return generation_; }
  std::uint64_t Deadline() const { return deadline_; }
  int ActivationAttempts() const { return activationAttempts_; }

 private:
  OverlayFocusPhase phase_ = OverlayFocusPhase::Idle;
  std::uint64_t generation_ = 0;
  std::uint64_t deadline_ = 0;
  int activationAttempts_ = 0;
};

struct OverlayRestoreIdentity {
  std::uintptr_t window = 0;
  std::uint32_t processId = 0;
  std::uint32_t threadId = 0;
  std::uint64_t generation = 0;
};

inline bool OverlayRestoreIdentityMatches(
    const OverlayRestoreIdentity& expected, std::uintptr_t currentWindow,
    std::uint32_t currentProcessId, std::uint32_t currentThreadId,
    std::uint64_t currentGeneration) {
  return expected.window != 0 && expected.window == currentWindow &&
         expected.processId == currentProcessId &&
         expected.threadId == currentThreadId &&
         expected.generation == currentGeneration;
}

inline bool ShouldAttemptOverlayRestore(bool explicitDismiss,
                                        bool overlayStillForeground,
                                        bool candidateValid) {
  return explicitDismiss && overlayStillForeground && candidateValid;
}

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
