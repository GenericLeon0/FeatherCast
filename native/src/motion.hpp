#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace feathercast::motion {

inline constexpr double kMaxFrameDeltaSeconds = 0.05;

inline double ClampFrameDeltaSeconds(double deltaSeconds) {
  return std::clamp(deltaSeconds, 0.0, kMaxFrameDeltaSeconds);
}

inline double EaseOutCubic(double progress) {
  const double t = std::clamp(progress, 0.0, 1.0);
  const double remaining = 1.0 - t;
  return 1.0 - remaining * remaining * remaining;
}

class ScalarAnimation {
 public:
  double Value() const { return value_; }
  double Target() const { return target_; }
  bool Active() const { return active_; }

  void Snap(double value) {
    value_ = value;
    start_ = value;
    target_ = value;
    elapsedSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    active_ = false;
  }

  void Retarget(double target, double durationSeconds, bool animate = true) {
    if (!animate || durationSeconds <= 0.0 ||
        std::abs(target - value_) <= 0.001) {
      Snap(target);
      return;
    }
    start_ = value_;
    target_ = target;
    elapsedSeconds_ = 0.0;
    durationSeconds_ = durationSeconds;
    active_ = true;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    elapsedSeconds_ += ClampFrameDeltaSeconds(deltaSeconds);
    const double progress = durationSeconds_ <= 0.0
                                ? 1.0
                                : elapsedSeconds_ / durationSeconds_;
    value_ = start_ + (target_ - start_) * EaseOutCubic(progress);
    if (progress >= 1.0 || std::abs(target_ - value_) <= 0.001) {
      Snap(target_);
    }
    return active_;
  }

 private:
  double value_ = 0.0;
  double start_ = 0.0;
  double target_ = 0.0;
  double elapsedSeconds_ = 0.0;
  double durationSeconds_ = 0.0;
  bool active_ = false;
};

class AnimationTimeline {
 public:
  double ElapsedSeconds() const { return elapsedSeconds_; }
  double DurationSeconds() const { return durationSeconds_; }
  bool Active() const { return active_; }

  void Start(double durationSeconds) {
    elapsedSeconds_ = 0.0;
    durationSeconds_ = std::max(0.0, durationSeconds);
    active_ = durationSeconds_ > 0.0;
  }

  void Reset() {
    elapsedSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    active_ = false;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    elapsedSeconds_ += ClampFrameDeltaSeconds(deltaSeconds);
    if (elapsedSeconds_ >= durationSeconds_) {
      elapsedSeconds_ = durationSeconds_;
      active_ = false;
    }
    return active_;
  }

 private:
  double elapsedSeconds_ = 0.0;
  double durationSeconds_ = 0.0;
  bool active_ = false;
};

struct AnimatedBounds {
  ScalarAnimation left;
  ScalarAnimation top;
  ScalarAnimation width;
  ScalarAnimation height;

  bool Active() const {
    return left.Active() || top.Active() || width.Active() || height.Active();
  }

  bool Update(double deltaSeconds) {
    left.Update(deltaSeconds);
    top.Update(deltaSeconds);
    width.Update(deltaSeconds);
    height.Update(deltaSeconds);
    return Active();
  }

  void Snap(double x, double y, double w, double h) {
    left.Snap(x);
    top.Snap(y);
    width.Snap(w);
    height.Snap(h);
  }

  void Retarget(double x, double y, double w, double h,
                double durationSeconds, bool animate) {
    left.Retarget(x, durationSeconds, animate);
    top.Retarget(y, durationSeconds, animate);
    width.Retarget(w, durationSeconds, animate);
    height.Retarget(h, durationSeconds, animate);
  }
};

struct Bounds {
  double left = 0.0;
  double top = 0.0;
  double width = 0.0;
  double height = 0.0;
};

inline Bounds OpeningBounds(const Bounds& target, double scale,
                            bool topAnchored) {
  const double clampedScale = std::clamp(scale, 0.01, 1.0);
  const double width = target.width * clampedScale;
  const double height = target.height * clampedScale;
  return {
      target.left + (target.width - width) * 0.5,
      topAnchored ? target.top
                  : target.top + (target.height - height) * 0.5,
      width,
      height,
  };
}

enum class AbsoluteNavigation { None, Home, End };

class PendingNavigation {
 public:
  bool Empty() const {
    return relative_ == 0 && absolute_ == AbsoluteNavigation::None;
  }

  void Clear() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::None;
  }

  void Move(int delta) {
    if (absolute_ != AbsoluteNavigation::None) absolute_ = AbsoluteNavigation::None;
    relative_ = std::clamp(relative_ + delta, -1000, 1000);
  }

  void Home() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::Home;
  }

  void End() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::End;
  }

  int Apply(int current, int itemCount) const {
    if (itemCount <= 0) return 0;
    if (absolute_ == AbsoluteNavigation::Home) return 0;
    if (absolute_ == AbsoluteNavigation::End) return itemCount - 1;
    return std::clamp(current + relative_, 0, itemCount - 1);
  }

 private:
  int relative_ = 0;
  AbsoluteNavigation absolute_ = AbsoluteNavigation::None;
};

inline double WheelDeltaPixels(int delta, double pixelsPerNotch = 72.0) {
  return static_cast<double>(delta) * pixelsPerNotch / 120.0;
}

}  // namespace feathercast::motion
