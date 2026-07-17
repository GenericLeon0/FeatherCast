#include "interaction_state.hpp"
#include "test_framework.hpp"

#include <string>

int main() {
  using feathercast::interaction::SearchPresentationState;
  using feathercast::interaction::StablePointerTargetMatches;

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

  return 0;
}
