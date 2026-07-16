#pragma once

#include <windows.h>
#include <oleacc.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::accessibility {

struct Item {
  std::wstring name;
  std::wstring value;
  std::wstring description;
  std::wstring defaultAction;
  LONG role = ROLE_SYSTEM_LISTITEM;
  LONG state = STATE_SYSTEM_FOCUSABLE;
  RECT screenRect{};
};

class Model {
 public:
  virtual ~Model() = default;
  virtual std::wstring AccessibleWindowName(HWND hwnd) const = 0;
  virtual std::vector<Item> AccessibleItems(HWND hwnd) const = 0;
  virtual int AccessibleFocusedChild(HWND hwnd) const = 0;  // one-based; 0 = root
  virtual void AccessibleFocusChild(HWND hwnd, int child) = 0;
  virtual void AccessibleInvokeChild(HWND hwnd, int child) = 0;
};

class Window final : public IAccessible {
 public:
  Window(Model* model, HWND hwnd) : model_(model), hwnd_(hwnd) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) {
      *object = static_cast<IAccessible*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
    if (!count) return E_POINTER;
    *count = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                                   EXCEPINFO*, UINT*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
    if (!parent) return E_POINTER;
    *parent = nullptr;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override {
    if (!count) return E_POINTER;
    *count = static_cast<LONG>(Items().size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override {
    if (!child) return E_POINTER;
    *child = nullptr;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
    if (!name) return E_POINTER;
    *name = nullptr;
    if (IsSelf(child)) {
      const std::wstring value = model_->AccessibleWindowName(hwnd_);
      *name = SysAllocString(value.c_str());
      return *name ? S_OK : E_OUTOFMEMORY;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    *name = SysAllocString(item->name.c_str());
    return *name ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->value.empty()) return S_FALSE;
    *value = SysAllocString(item->value.c_str());
    return *value ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override {
    if (!description) return E_POINTER;
    *description = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->description.empty()) return S_FALSE;
    *description = SysAllocString(item->description.c_str());
    return *description ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
    if (!role) return E_POINTER;
    VariantInit(role);
    role->vt = VT_I4;
    if (IsSelf(child)) {
      role->lVal = ROLE_SYSTEM_WINDOW;
      return S_OK;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    role->lVal = item->role;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
    if (!state) return E_POINTER;
    VariantInit(state);
    state->vt = VT_I4;
    if (IsSelf(child)) {
      state->lVal = IsWindowVisible(hwnd_) ? 0 : STATE_SYSTEM_INVISIBLE;
      return S_OK;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    state->lVal = item->state;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, LONG*) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
    if (!focus) return E_POINTER;
    VariantInit(focus);
    focus->vt = VT_I4;
    focus->lVal = model_->AccessibleFocusedChild(hwnd_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
    return get_accFocus(selection);
  }
  HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
    if (!action) return E_POINTER;
    *action = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->defaultAction.empty()) return S_FALSE;
    *action = SysAllocString(item->defaultAction.c_str());
    return *action ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE accSelect(LONG flags, VARIANT child) override {
    const auto id = ChildId(child);
    if (!id) return E_INVALIDARG;
    if ((flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION)) != 0) {
      model_->AccessibleFocusChild(hwnd_, *id);
      return S_OK;
    }
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE accLocation(LONG* left, LONG* top, LONG* width, LONG* height,
                                        VARIANT child) override {
    if (!left || !top || !width || !height) return E_POINTER;
    RECT rect{};
    if (IsSelf(child)) {
      GetWindowRect(hwnd_, &rect);
    } else {
      const auto item = ItemFor(child);
      if (!item) return E_INVALIDARG;
      rect = item->screenRect;
    }
    *left = rect.left;
    *top = rect.top;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accNavigate(LONG direction, VARIANT start, VARIANT* destination) override {
    if (!destination) return E_POINTER;
    VariantInit(destination);
    const LONG count = static_cast<LONG>(Items().size());
    LONG id = IsSelf(start) ? 0 : start.lVal;
    if (direction == NAVDIR_FIRSTCHILD && id == 0 && count > 0) id = 1;
    else if (direction == NAVDIR_LASTCHILD && id == 0 && count > 0) id = count;
    else if (direction == NAVDIR_NEXT && id > 0 && id < count) ++id;
    else if (direction == NAVDIR_PREVIOUS && id > 1) --id;
    else return S_FALSE;
    destination->vt = VT_I4;
    destination->lVal = id;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accHitTest(LONG x, LONG y, VARIANT* child) override {
    if (!child) return E_POINTER;
    VariantInit(child);
    const auto items = Items();
    for (size_t i = 0; i < items.size(); ++i) {
      POINT point{x, y};
      if (PtInRect(&items[i].screenRect, point)) {
        child->vt = VT_I4;
        child->lVal = static_cast<LONG>(i + 1);
        return S_OK;
      }
    }
    child->vt = VT_I4;
    child->lVal = CHILDID_SELF;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
    const auto id = ChildId(child);
    if (!id) return E_INVALIDARG;
    model_->AccessibleInvokeChild(hwnd_, *id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

 private:
  static bool IsSelf(const VARIANT& child) {
    return child.vt == VT_I4 && child.lVal == CHILDID_SELF;
  }
  static std::optional<int> ChildId(const VARIANT& child) {
    if (child.vt != VT_I4 || child.lVal <= 0) return std::nullopt;
    return static_cast<int>(child.lVal);
  }
  std::vector<Item> Items() const { return model_->AccessibleItems(hwnd_); }
  std::optional<Item> ItemFor(const VARIANT& child) const {
    const auto id = ChildId(child);
    if (!id) return std::nullopt;
    const auto items = Items();
    if (*id > static_cast<int>(items.size())) return std::nullopt;
    return items[static_cast<size_t>(*id - 1)];
  }

  std::atomic<ULONG> references_ = 1;
  Model* model_ = nullptr;
  HWND hwnd_ = nullptr;
};

inline LRESULT HandleGetObject(Model* model, HWND hwnd, WPARAM wParam, LPARAM lParam) {
  if (static_cast<LONG>(lParam) != OBJID_CLIENT) return 0;
  auto* accessible = new Window(model, hwnd);
  const LRESULT result = LresultFromObject(IID_IAccessible, wParam, accessible);
  accessible->Release();
  return result;
}

}  // namespace feathercast::accessibility
