#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace feathercast::audio {

inline int ClampPercent(int percent) {
  return std::clamp(percent, 0, 100);
}

inline int AdjustPercent(int percent, int delta) {
  return ClampPercent(percent + delta);
}

inline int PercentFromTrack(float x, float left, float right) {
  if (right <= left) return 0;
  const float position = std::clamp((x - left) / (right - left), 0.0f, 1.0f);
  return ClampPercent(static_cast<int>(std::lround(position * 100.0f)));
}

std::optional<int> ReadDefaultOutputVolumePercent();
bool SetDefaultOutputVolumePercent(int percent);
bool StepDefaultOutputVolume(bool increase);
bool ToggleDefaultOutputMute();

}  // namespace feathercast::audio
