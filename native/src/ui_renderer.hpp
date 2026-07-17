#pragma once

#include "app_types.hpp"

#include <d2d1_1.h>

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace feathercast::ui {

class HitRegions {
 public:
  using Container = std::vector<app::HitTarget>;
  using iterator = Container::iterator;
  using const_iterator = Container::const_iterator;

  void clear() noexcept { regions_.clear(); }
  void push_back(app::HitTarget region) {
    regions_.push_back(std::move(region));
  }
  [[nodiscard]] iterator begin() noexcept { return regions_.begin(); }
  [[nodiscard]] iterator end() noexcept { return regions_.end(); }
  [[nodiscard]] const_iterator begin() const noexcept { return regions_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return regions_.end(); }
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return regions_.size(); }

  [[nodiscard]] const app::HitTarget* At(float x, float y) const noexcept;

 private:
  Container regions_;
};

struct RenderFrameResult {
  HRESULT result = E_FAIL;
  bool recreateTarget = false;
};

// Owns the render pass boundary. The callback emits the view-model-specific
// drawing commands while this class consistently handles begin/clear/end and
// device-loss classification.
RenderFrameResult RenderTransparentFrame(
    ID2D1DeviceContext* context, const std::function<void()>& draw);

}  // namespace feathercast::ui
