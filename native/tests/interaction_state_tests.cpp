#include "interaction_state.hpp"
#include "motion.hpp"
#include "test_framework.hpp"

#include <string>

int main() {
  using feathercast::interaction::OverlayFocusPhase;
  using feathercast::interaction::OverlayFocusSession;
  using feathercast::interaction::OverlayRestoreIdentity;
  using feathercast::interaction::OverlayRestoreIdentityMatches;
  using feathercast::interaction::SearchPresentationState;
  using feathercast::interaction::ShouldAttemptOverlayRestore;
  using feathercast::interaction::StablePointerTargetMatches;

  {
    OverlayFocusSession focus;
    const auto first = focus.Begin(1000);
    assert(first == 1);
    assert(focus.Phase() == OverlayFocusPhase::Acquiring);
    assert(focus.GuardActive(1000));
    assert(focus.AcceptsExternalCandidate(1050));

    focus.RecordActivationAttempt(false);
    assert(focus.ActivationAttempts() == 1);
    assert(focus.Phase() == OverlayFocusPhase::Acquiring);
    focus.RecordActivationAttempt(true);
    assert(focus.ActivationAttempts() == 2);
    assert(focus.Phase() == OverlayFocusPhase::Stabilizing);
    assert(focus.AcceptsExternalCandidate(1150));
    assert(focus.AcceptsExternalCandidate(1250));
    assert(!focus.AcceptsExternalCandidate(1300));

    focus.Expire(1300);
    assert(focus.Phase() == OverlayFocusPhase::Idle);
    focus.BeginClosing();
    assert(focus.CanFinishClose(first));
    const auto second = focus.Begin(1400);
    assert(second == 2);
    assert(!focus.CanFinishClose(first));
    focus.BeginClosing();
    assert(focus.CanFinishClose(second));
    focus.End();
    assert(focus.Phase() == OverlayFocusPhase::Idle);
  }

  {
    const OverlayRestoreIdentity expected{0x1234, 10, 20, 7};
    assert(OverlayRestoreIdentityMatches(expected, 0x1234, 10, 20, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 11, 20, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 10, 21, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 10, 20, 8));
    assert(!OverlayRestoreIdentityMatches(expected, 0, 10, 20, 7));

    assert(ShouldAttemptOverlayRestore(true, true, true));
    assert(!ShouldAttemptOverlayRestore(false, true, true));
    assert(!ShouldAttemptOverlayRestore(true, false, true));
    assert(!ShouldAttemptOverlayRestore(true, true, false));
  }

  {
    SearchPresentationState state;
    assert(!state.Pending());
    assert(state.CanActivate());

    const auto first = state.NextRequest();
    assert(first == 1);
    assert(state.Pending());
    assert(!state.CanActivate());

    state.MarkCompleted(first);
    assert(!state.CanActivate());
    assert(state.Publish(first));
    assert(!state.Pending());
    assert(state.CanActivate());

    const auto second = state.NextRequest();
    const auto third = state.NextRequest();
    assert(second == 2 && third == 3);
    state.MarkCompleted(second);
    assert(!state.Publish(second));
    assert(state.Pending());
    assert(!state.CanActivate());
    assert(state.Publish(third));
    assert(state.CanActivate());

    state.Invalidate();
    assert(state.Pending());
    assert(!state.CanActivate());
  }

  {
    const std::wstring key = L"app:notepad";
    assert(StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, true,
                                      true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, false,
                                       true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 10, key, true,
                                       true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9,
                                       L"app:calculator", true, true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, true,
                                       false));
  }

  {
    feathercast::motion::ScalarAnimation animation;
    animation.Snap(10.0);
    animation.Retarget(20.0, 0.1);
    assert(animation.Active());
    animation.Update(0.05);
    assert(animation.Value() > 10.0 && animation.Value() < 20.0);

    const double interrupted = animation.Value();
    animation.Retarget(5.0, 0.1);
    assert(animation.Value() == interrupted);
    animation.Update(0.05);
    animation.Update(0.05);
    assert(!animation.Active());
    assert(std::abs(animation.Value() - 5.0) < 0.001);

    animation.Retarget(30.0, 1.0, false);
    assert(!animation.Active() && animation.Value() == 30.0);
  }

  {
    feathercast::motion::AnimationTimeline timeline;
    timeline.Start(0.21);
    assert(timeline.Active());
    timeline.Update(2.0);
    assert(timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds() -
                    feathercast::motion::kMaxFrameDeltaSeconds) < 0.001);

    timeline.Update(0.05);
    timeline.Start(0.21);
    assert(timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds()) < 0.001);

    timeline.Update(0.05);
    timeline.Update(0.05);
    timeline.Update(0.05);
    timeline.Update(0.05);
    assert(timeline.Active());
    timeline.Update(0.05);
    assert(!timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds() - 0.21) < 0.001);

    timeline.Reset();
    assert(!timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds()) < 0.001);
  }

  {
    feathercast::motion::AnimatedBounds bounds;
    bounds.Snap(100.0, 200.0, 400.0, 300.0);
    bounds.Retarget(90.0, 190.0, 420.0, 320.0, 0.16, true);
    bounds.Update(0.08);
    assert(bounds.left.Value() < 100.0 && bounds.left.Value() > 90.0);
    assert(bounds.width.Value() > 400.0 && bounds.width.Value() < 420.0);
    bounds.Update(0.05);
    bounds.Update(0.05);
    bounds.Update(0.05);
    assert(!bounds.Active());

    const feathercast::motion::Bounds target{100.0, 200.0, 400.0, 300.0};
    const auto centered =
        feathercast::motion::OpeningBounds(target, 0.98, false);
    assert(std::abs(centered.left - 104.0) < 0.001 &&
           std::abs(centered.top - 203.0) < 0.001);
    const auto topAnchored =
        feathercast::motion::OpeningBounds(target, 0.98, true);
    assert(std::abs(topAnchored.left - 104.0) < 0.001 &&
           std::abs(topAnchored.top - 200.0) < 0.001);
  }

  {
    feathercast::motion::PendingNavigation pending;
    pending.Move(1);
    pending.Move(8);
    assert(pending.Apply(0, 20) == 9);
    pending.Home();
    assert(pending.Apply(12, 20) == 0);
    pending.End();
    assert(pending.Apply(2, 20) == 19);
    pending.Clear();
    assert(pending.Empty());
    assert(feathercast::motion::WheelDeltaPixels(30) == 18.0);
  }

  return 0;
}
