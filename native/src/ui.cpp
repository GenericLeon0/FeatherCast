#include "app_types.hpp"
#include "ui_renderer.hpp"

#include <d2d1helper.h>

namespace feathercast::ui {

void UiLibraryAnchor() {}

const app::HitTarget* HitRegions::At(float x, float y) const noexcept {
  for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
    if (x >= it->rect.left && x <= it->rect.right &&
        y >= it->rect.top && y <= it->rect.bottom) {
      return &*it;
    }
  }
  return nullptr;
}

RenderFrameResult RenderTransparentFrame(
    ID2D1DeviceContext* context, const std::function<void()>& draw) {
  if (!context) return {};
  context->BeginDraw();
  context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  draw();
  const HRESULT result = context->EndDraw();
  return {result, result == D2DERR_RECREATE_TARGET};
}

}  // namespace feathercast::ui
