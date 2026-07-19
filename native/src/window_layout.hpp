#pragma once

#include <algorithm>
#include <cstddef>

namespace feathercast::window_layout {

enum class Layout {
  LeftHalf,
  RightHalf,
  TopHalf,
  BottomHalf,
  LeftThird,
  CenterThird,
  RightThird,
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
  Center,
  PreviousDisplay,
  NextDisplay,
};

struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  [[nodiscard]] int Width() const { return std::max(0, right - left); }
  [[nodiscard]] int Height() const { return std::max(0, bottom - top); }
  bool operator==(const Rect&) const = default;
};

inline bool PositionLess(const Rect& left, const Rect& right) {
  return left.left != right.left ? left.left < right.left
                                 : left.top < right.top;
}

inline std::size_t NextIndex(std::size_t current, std::size_t count) {
  return count == 0 ? 0 : (current + 1) % count;
}

inline std::size_t PreviousIndex(std::size_t current, std::size_t count) {
  return count == 0 ? 0 : (current + count - 1) % count;
}

inline Rect ClampSizeAndCenter(Rect window, const Rect& workArea) {
  const int width = std::min(window.Width(), workArea.Width());
  const int height = std::min(window.Height(), workArea.Height());
  const int left = workArea.left + (workArea.Width() - width) / 2;
  const int top = workArea.top + (workArea.Height() - height) / 2;
  return {left, top, left + width, top + height};
}

inline Rect Compute(Layout layout, const Rect& window,
                    const Rect& sourceWorkArea,
                    const Rect& targetWorkArea) {
  const int width = sourceWorkArea.Width();
  const int height = sourceWorkArea.Height();
  const int middleX = sourceWorkArea.left + width / 2;
  const int middleY = sourceWorkArea.top + height / 2;
  const int firstThirdX = sourceWorkArea.left + width / 3;
  const int secondThirdX = sourceWorkArea.left + (width * 2) / 3;

  switch (layout) {
    case Layout::LeftHalf:
      return {sourceWorkArea.left, sourceWorkArea.top, middleX,
              sourceWorkArea.bottom};
    case Layout::RightHalf:
      return {middleX, sourceWorkArea.top, sourceWorkArea.right,
              sourceWorkArea.bottom};
    case Layout::TopHalf:
      return {sourceWorkArea.left, sourceWorkArea.top, sourceWorkArea.right,
              middleY};
    case Layout::BottomHalf:
      return {sourceWorkArea.left, middleY, sourceWorkArea.right,
              sourceWorkArea.bottom};
    case Layout::LeftThird:
      return {sourceWorkArea.left, sourceWorkArea.top, firstThirdX,
              sourceWorkArea.bottom};
    case Layout::CenterThird:
      return {firstThirdX, sourceWorkArea.top, secondThirdX,
              sourceWorkArea.bottom};
    case Layout::RightThird:
      return {secondThirdX, sourceWorkArea.top, sourceWorkArea.right,
              sourceWorkArea.bottom};
    case Layout::TopLeft:
      return {sourceWorkArea.left, sourceWorkArea.top, middleX, middleY};
    case Layout::TopRight:
      return {middleX, sourceWorkArea.top, sourceWorkArea.right, middleY};
    case Layout::BottomLeft:
      return {sourceWorkArea.left, middleY, middleX, sourceWorkArea.bottom};
    case Layout::BottomRight:
      return {middleX, middleY, sourceWorkArea.right, sourceWorkArea.bottom};
    case Layout::Center:
      return ClampSizeAndCenter(window, sourceWorkArea);
    case Layout::PreviousDisplay:
    case Layout::NextDisplay: {
      const int targetWidth = std::min(window.Width(), targetWorkArea.Width());
      const int targetHeight = std::min(window.Height(), targetWorkArea.Height());
      const double normalizedCenterX =
          sourceWorkArea.Width() > 0
              ? (static_cast<double>(window.left + window.right) / 2.0 -
                 sourceWorkArea.left) /
                    sourceWorkArea.Width()
              : 0.5;
      const double normalizedCenterY =
          sourceWorkArea.Height() > 0
              ? (static_cast<double>(window.top + window.bottom) / 2.0 -
                 sourceWorkArea.top) /
                    sourceWorkArea.Height()
              : 0.5;
      int left = targetWorkArea.left +
                 static_cast<int>(normalizedCenterX * targetWorkArea.Width()) -
                 targetWidth / 2;
      int top = targetWorkArea.top +
                static_cast<int>(normalizedCenterY * targetWorkArea.Height()) -
                targetHeight / 2;
      left = std::clamp(left, targetWorkArea.left,
                        targetWorkArea.right - targetWidth);
      top = std::clamp(top, targetWorkArea.top,
                       targetWorkArea.bottom - targetHeight);
      return {left, top, left + targetWidth, top + targetHeight};
    }
  }
  return window;
}

}  // namespace feathercast::window_layout
