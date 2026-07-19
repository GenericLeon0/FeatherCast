#include "library_ui.hpp"

#include <commctrl.h>
#include <initguid.h>
#include <oleacc.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>

namespace feathercast::library_ui {
namespace {

constexpr wchar_t kManagerClass[] = L"FeatherCastLibraryManager";
constexpr wchar_t kEditorClass[] = L"FeatherCastLibraryEditor";

enum ControlId : int {
  IdTabs = 100,
  IdList,
  IdAdd,
  IdEdit,
  IdDelete,
  IdReload,
  IdOpenFile,
  IdStatus,
  IdName,
  IdKeyword,
  IdValue,
};

void UseDefaultFont(HWND control) {
  SendMessageW(control, WM_SETFONT,
               reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void SetAccessibleName(HWND control, const wchar_t* name) {
  IAccPropServices* services = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&services)))) {
    services->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT),
                             CHILDID_SELF,
                             PROPID_ACC_NAME, name);
    services->Release();
  }
}

std::wstring ControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
  if (length > 0) GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<std::size_t>(std::max(0, length)));
  return value;
}

void CenterOwnedWindow(HWND window, HWND owner) {
  RECT windowRect{};
  RECT ownerRect{};
  GetWindowRect(window, &windowRect);
  if (!owner || !GetWindowRect(owner, &ownerRect)) {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
  }
  const int width = windowRect.right - windowRect.left;
  const int height = windowRect.bottom - windowRect.top;
  const int left = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
  const int top = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2;
  SetWindowPos(window, nullptr, left, top, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

class EditorWindow {
 public:
  EditorWindow(HWND owner, library::ItemKind kind,
               const std::vector<snippets::Snippet>& snippets,
               const std::vector<settings::Quicklink>& quicklinks,
               const std::vector<library::AppAlias>& aliases,
               const std::vector<library::AppChoice>& availableApps,
               const std::vector<library::WebSearch>& searches,
               std::optional<std::size_t> editingIndex,
               std::wstring preferredAppId = {})
      : owner_(owner),
        kind_(kind),
        snippets_(snippets),
        quicklinks_(quicklinks),
        aliases_(aliases),
        availableApps_(availableApps),
        searches_(searches),
        editingIndex_(editingIndex),
        preferredAppId_(std::move(preferredAppId)) {
    if (editingIndex_) {
      if (kind_ == library::ItemKind::Snippet) {
        snippet_ = snippets_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::Quicklink) {
        quicklink_ = quicklinks_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::AppAlias) {
        alias_ = aliases_.at(*editingIndex_);
      } else {
        webSearch_ = searches_.at(*editingIndex_);
      }
    }
  }

  bool Run() {
    Register();
    const wchar_t* title = L"Edit Library Item";
    if (kind_ == library::ItemKind::Snippet) {
      title = editingIndex_ ? L"Edit Snippet" : L"Add Snippet";
    } else if (kind_ == library::ItemKind::Quicklink) {
      title = editingIndex_ ? L"Edit Quicklink" : L"Add Quicklink";
    } else if (kind_ == library::ItemKind::AppAlias) {
      title = editingIndex_ ? L"Edit App Alias" : L"Add App Alias";
    } else {
      title = editingIndex_ ? L"Edit Web Search" : L"Add Web Search";
    }
    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kEditorClass, title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 570,
        kind_ == library::ItemKind::Snippet ? 430 : 285, owner_, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG message{};
    while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
        continue;
      }
      if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
          (kind_ != library::ItemKind::Snippet || GetFocus() != value_ ||
           (GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
        SendMessageW(hwnd_, WM_COMMAND, IDOK, 0);
        continue;
      }
      if (!IsDialogMessageW(hwnd_, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
    return accepted_;
  }

  const snippets::Snippet& Snippet() const { return snippet_; }
  const settings::Quicklink& Quicklink() const { return quicklink_; }
  const library::AppAlias& Alias() const { return alias_; }
  const library::WebSearch& WebSearch() const { return webSearch_; }

 private:
  static void Register() {
    static const bool registered = [] {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = &EditorWindow::WindowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
      wc.lpszClassName = kEditorClass;
      return RegisterClassExW(&wc) != 0 ||
             GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    auto* self = reinterpret_cast<EditorWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<EditorWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
      self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
                  int id) {
    HWND control = CreateWindowExW(
        wcscmp(className, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0, className, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    UseDefaultFont(control);
    return control;
  }

  void CreateControls() {
    const bool alias = kind_ == library::ItemKind::AppAlias;
    const bool webSearch = kind_ == library::ItemKind::WebSearch;
    nameLabel_ = AddControl(
        WC_STATICW,
        alias ? L"App" :
        (kind_ == library::ItemKind::Snippet ? L"Name" : L"Name (optional)"),
        SS_LEFT, 0);
    name_ = AddControl(alias ? WC_COMBOBOXW : WC_EDITW, L"",
                       WS_TABSTOP | (alias ? CBS_DROPDOWNLIST | WS_VSCROLL
                                           : ES_AUTOHSCROLL),
                       IdName);
    keywordLabel_ = AddControl(WC_STATICW, alias ? L"Alias" : L"Keyword",
                               SS_LEFT, 0);
    keyword_ =
        AddControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IdKeyword);
    valueLabel_ = AddControl(
        WC_STATICW,
        kind_ == library::ItemKind::Snippet ? L"Text" :
        (webSearch ? L"URL template (use %s for the query)" : L"Target"),
        SS_LEFT, 0);
    DWORD valueStyle = WS_TABSTOP;
    if (kind_ == library::ItemKind::Snippet) {
      valueStyle |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
    } else {
      valueStyle |= ES_AUTOHSCROLL;
    }
    value_ = AddControl(WC_EDITW, L"", valueStyle, IdValue);
    save_ = AddControl(WC_BUTTONW, L"Save",
                       WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = AddControl(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
    SetAccessibleName(name_, alias ? L"App" :
        (kind_ == library::ItemKind::Snippet ? L"Snippet name"
                                             : L"Quicklink name (optional)"));
    SetAccessibleName(keyword_, alias ? L"App alias" : L"Keyword");
    SetAccessibleName(value_, webSearch ? L"Web search URL template" :
        (kind_ == library::ItemKind::Snippet ? L"Snippet text"
                                             : L"Quicklink target"));

    if (kind_ == library::ItemKind::Snippet) {
      SetWindowTextW(name_, snippet_.name.c_str());
      SetWindowTextW(keyword_, snippet_.keyword.c_str());
      SetWindowTextW(value_, snippet_.text.c_str());
    } else if (kind_ == library::ItemKind::Quicklink) {
      SetWindowTextW(name_, quicklink_.name.c_str());
      SetWindowTextW(keyword_, quicklink_.keyword.c_str());
      SetWindowTextW(value_, quicklink_.target.c_str());
    } else if (alias) {
      std::wstring selectedId = alias_.appId.empty() ? preferredAppId_
                                                     : alias_.appId;
      int selected = -1;
      for (std::size_t index = 0; index < availableApps_.size(); ++index) {
        const auto& app = availableApps_[index];
        const int item = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(app.name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, item,
                     static_cast<LPARAM>(index));
        if (app.id == selectedId) selected = item;
      }
      if (selected < 0 && !selectedId.empty()) {
        library::AppChoice missing{selectedId, L"Missing app - " + selectedId};
        availableApps_.push_back(std::move(missing));
        selected = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(availableApps_.back().name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, selected,
                     static_cast<LPARAM>(availableApps_.size() - 1));
      }
      if (selected < 0 && !availableApps_.empty()) selected = 0;
      SendMessageW(name_, CB_SETCURSEL, selected, 0);
      SetWindowTextW(keyword_, alias_.alias.c_str());
      ShowWindow(valueLabel_, SW_HIDE);
      ShowWindow(value_, SW_HIDE);
    } else {
      ShowWindow(nameLabel_, SW_HIDE);
      ShowWindow(name_, SW_HIDE);
      SetWindowTextW(keyword_, webSearch_.keyword.c_str());
      SetWindowTextW(value_, webSearch_.urlTemplate.c_str());
    }
    SetFocus(webSearch ? keyword_ : name_);
  }

  void Layout(int width, int height) const {
    constexpr int margin = 18;
    constexpr int labelHeight = 20;
    constexpr int editHeight = 27;
    constexpr int gap = 12;
    int y = margin;
    if (kind_ != library::ItemKind::WebSearch) {
      MoveWindow(nameLabel_, margin, y, width - 2 * margin, labelHeight, TRUE);
      y += labelHeight;
      MoveWindow(name_, margin, y, width - 2 * margin,
                 kind_ == library::ItemKind::AppAlias ? 240 : editHeight, TRUE);
      y += editHeight + gap;
    }
    MoveWindow(keywordLabel_, margin, y, width - 2 * margin, labelHeight, TRUE);
    y += labelHeight;
    MoveWindow(keyword_, margin, y, width - 2 * margin, editHeight, TRUE);
    y += editHeight + gap;
    if (kind_ == library::ItemKind::AppAlias) {
      MoveWindow(cancel_, width - margin - 90, height - margin - 30, 90, 30,
                 TRUE);
      MoveWindow(save_, width - margin - 90 - gap - 90,
                 height - margin - 30, 90, 30, TRUE);
      return;
    }
    MoveWindow(valueLabel_, margin, y, width - 2 * margin, labelHeight, TRUE);
    y += labelHeight;
    const int buttonsTop = height - margin - 30;
    MoveWindow(value_, margin, y, width - 2 * margin,
               std::max(editHeight, buttonsTop - y - gap), TRUE);
    MoveWindow(cancel_, width - margin - 90, buttonsTop, 90, 30, TRUE);
    MoveWindow(save_, width - margin - 90 - gap - 90, buttonsTop, 90, 30,
               TRUE);
  }

  void Accept() {
    if (kind_ == library::ItemKind::Snippet) {
      snippet_.name = snippets::Trim(ControlText(name_));
      snippet_.keyword = snippets::Trim(ControlText(keyword_));
      snippet_.text = ControlText(value_);
      if (const auto error = library::ValidateSnippet(
              snippet_, snippets_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Snippet",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else if (kind_ == library::ItemKind::Quicklink) {
      quicklink_.name = snippets::Trim(ControlText(name_));
      quicklink_.keyword = snippets::Trim(ControlText(keyword_));
      quicklink_.target = snippets::Trim(ControlText(value_));
      if (const auto error = library::ValidateQuicklink(
              quicklink_, quicklinks_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Quicklink",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else if (kind_ == library::ItemKind::AppAlias) {
      const int selected = static_cast<int>(SendMessageW(name_, CB_GETCURSEL, 0, 0));
      if (selected >= 0) {
        const auto index = static_cast<std::size_t>(SendMessageW(
            name_, CB_GETITEMDATA, selected, 0));
        if (index < availableApps_.size()) {
          alias_.appId = availableApps_[index].id;
          alias_.appName = availableApps_[index].name;
        }
      }
      alias_.alias = snippets::Trim(ControlText(keyword_));
      if (const auto error = library::ValidateAppAlias(
              alias_, aliases_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid App Alias",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else {
      webSearch_.keyword = library::NormalizeKeyword(ControlText(keyword_));
      webSearch_.urlTemplate = snippets::Trim(ControlText(value_));
      if (const auto error = library::ValidateWebSearch(
              webSearch_, searches_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Web Search",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    }
    accepted_ = true;
    DestroyWindow(hwnd_);
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
      case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
      case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
          Accept();
          return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
          DestroyWindow(hwnd_);
          return 0;
        }
        break;
      case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
  }

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  library::ItemKind kind_ = library::ItemKind::Snippet;
  std::vector<snippets::Snippet> snippets_;
  std::vector<settings::Quicklink> quicklinks_;
  std::vector<library::AppAlias> aliases_;
  std::vector<library::AppChoice> availableApps_;
  std::vector<library::WebSearch> searches_;
  std::optional<std::size_t> editingIndex_;
  snippets::Snippet snippet_;
  settings::Quicklink quicklink_;
  library::AppAlias alias_;
  library::WebSearch webSearch_;
  std::wstring preferredAppId_;
  bool accepted_ = false;
  HWND nameLabel_ = nullptr;
  HWND name_ = nullptr;
  HWND keywordLabel_ = nullptr;
  HWND keyword_ = nullptr;
  HWND valueLabel_ = nullptr;
  HWND value_ = nullptr;
  HWND save_ = nullptr;
  HWND cancel_ = nullptr;
};

class ManagerWindow {
 public:
  ManagerWindow(HWND owner, ManagerData data, ManagerCallbacks callbacks,
                library::ItemKind initialKind, std::wstring initialAppId)
      : owner_(owner),
        data_(std::move(data)),
        callbacks_(std::move(callbacks)),
        kind_(initialKind),
        initialAppId_(std::move(initialAppId)) {}

  void Run() {
    Register();
    INITCOMMONCONTROLSEX controls{sizeof(controls),
                                  ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kManagerClass,
        L"FeatherCast Library",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 530, owner_, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG message{};
    while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
        continue;
      }
      if (!IsDialogMessageW(hwnd_, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
  }

 private:
  static void Register() {
    static const bool registered = [] {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = &ManagerWindow::WindowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
      wc.lpszClassName = kManagerClass;
      return RegisterClassExW(&wc) != 0 ||
             GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    auto* self = reinterpret_cast<ManagerWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<ManagerWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
      self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
                  int id) {
    HWND control = CreateWindowExW(
        wcscmp(className, WC_LISTVIEWW) == 0 ? WS_EX_CLIENTEDGE : 0, className, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    UseDefaultFont(control);
    return control;
  }

  void CreateControls() {
    tabs_ = AddControl(WC_TABCONTROLW, L"", WS_TABSTOP, IdTabs);
    TCITEMW tab{TCIF_TEXT};
    tab.pszText = const_cast<wchar_t*>(L"Snippets");
    TabCtrl_InsertItem(tabs_, 0, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Quicklinks");
    TabCtrl_InsertItem(tabs_, 1, &tab);
    tab.pszText = const_cast<wchar_t*>(L"App Aliases");
    TabCtrl_InsertItem(tabs_, 2, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Web Searches");
    TabCtrl_InsertItem(tabs_, 3, &tab);
    TabCtrl_SetCurSel(tabs_, static_cast<int>(kind_));

    list_ = AddControl(WC_LISTVIEWW, L"",
                       WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                           LVS_SHOWSELALWAYS,
                       IdList);
    SetAccessibleName(list_, L"Library items");
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.pszText = const_cast<wchar_t*>(L"Name");
    column.cx = 190;
    ListView_InsertColumn(list_, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"Keyword");
    column.cx = 130;
    ListView_InsertColumn(list_, 1, &column);
    column.pszText = const_cast<wchar_t*>(L"Text / Target");
    column.cx = 360;
    ListView_InsertColumn(list_, 2, &column);

    add_ = AddControl(WC_BUTTONW, L"Add", WS_TABSTOP, IdAdd);
    edit_ = AddControl(WC_BUTTONW, L"Edit", WS_TABSTOP, IdEdit);
    remove_ = AddControl(WC_BUTTONW, L"Delete", WS_TABSTOP, IdDelete);
    reload_ = AddControl(WC_BUTTONW, L"Reload", WS_TABSTOP, IdReload);
    openFile_ =
        AddControl(WC_BUTTONW, L"Open File", WS_TABSTOP, IdOpenFile);
    close_ = AddControl(WC_BUTTONW, L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON,
                        IDCANCEL);
    status_ = AddControl(WC_STATICW, L"", SS_LEFT, IdStatus);
    Refresh();
    if (kind_ == library::ItemKind::AppAlias && !initialAppId_.empty()) {
      const auto alias = std::find_if(
          data_.appAliases.begin(), data_.appAliases.end(),
          [&](const auto& item) { return item.appId == initialAppId_; });
      if (alias == data_.appAliases.end()) {
        AddItem();
      } else {
        const auto sourceIndex = static_cast<LPARAM>(
            std::distance(data_.appAliases.begin(), alias));
        for (int row = 0; row < ListView_GetItemCount(list_); ++row) {
          LVITEMW item{LVIF_PARAM};
          item.iItem = row;
          if (ListView_GetItem(list_, &item) && item.lParam == sourceIndex) {
            ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            EditItem();
            break;
          }
        }
      }
      initialAppId_.clear();
    }
  }

  void Layout(int width, int height) const {
    constexpr int margin = 14;
    constexpr int tabHeight = 30;
    constexpr int buttonWidth = 90;
    constexpr int buttonHeight = 30;
    constexpr int gap = 8;
    MoveWindow(tabs_, margin, margin, width - 2 * margin, tabHeight, TRUE);
    const int buttonsTop = height - margin - buttonHeight;
    const int statusTop = buttonsTop - 28;
    MoveWindow(list_, margin, margin + tabHeight + 6, width - 2 * margin,
               statusTop - (margin + tabHeight + 12), TRUE);
    MoveWindow(status_, margin, statusTop, width - 2 * margin, 22, TRUE);
    int x = margin;
    for (HWND button : {add_, edit_, remove_, reload_}) {
      MoveWindow(button, x, buttonsTop, buttonWidth, buttonHeight, TRUE);
      x += buttonWidth + gap;
    }
    constexpr int specialButtonWidth = 118;
    MoveWindow(openFile_, x, buttonsTop, specialButtonWidth, buttonHeight,
               TRUE);
    MoveWindow(close_, width - margin - buttonWidth, buttonsTop, buttonWidth,
               buttonHeight, TRUE);
  }

  bool Writable() const {
    if (kind_ == library::ItemKind::Snippet) return data_.snippetsWritable;
    if (kind_ == library::ItemKind::Quicklink) return data_.quicklinksWritable;
    return data_.settingsWritable;
  }

  std::optional<std::size_t> SelectedIndex() const {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (selected < 0) return std::nullopt;
    LVITEMW item{LVIF_PARAM};
    item.iItem = selected;
    if (!ListView_GetItem(list_, &item) || item.lParam < 0) return std::nullopt;
    return static_cast<std::size_t>(item.lParam);
  }

  void InsertRow(int row, std::size_t sourceIndex, const std::wstring& name,
                 const std::wstring& keyword, const std::wstring& detail) {
    LVITEMW item{LVIF_TEXT | LVIF_PARAM};
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(name.c_str());
    item.lParam = static_cast<LPARAM>(sourceIndex);
    const int inserted = ListView_InsertItem(list_, &item);
    ListView_SetItemText(list_, inserted, 1,
                         const_cast<wchar_t*>(keyword.c_str()));
    ListView_SetItemText(list_, inserted, 2,
                         const_cast<wchar_t*>(detail.c_str()));
  }

  void Refresh() {
    ListView_DeleteAllItems(list_);
    if (kind_ == library::ItemKind::Snippet) {
      int row = 0;
      for (const auto index : library::SortedSnippetIndices(data_.snippets)) {
        auto preview = data_.snippets[index].text;
        std::replace(preview.begin(), preview.end(), L'\n', L' ');
        std::replace(preview.begin(), preview.end(), L'\r', L' ');
        const auto keyword =
            library::NormalizeKeyword(data_.snippets[index].keyword);
        const auto duplicates = std::count_if(
            data_.snippets.begin(), data_.snippets.end(), [&](const auto& item) {
              return library::NormalizeKeyword(item.keyword) == keyword;
            });
        if (duplicates > 1) preview = L"[Duplicate keyword] " + preview;
        InsertRow(row++, index, data_.snippets[index].name,
                  data_.snippets[index].keyword, preview);
      }
    } else if (kind_ == library::ItemKind::Quicklink) {
      int row = 0;
      for (const auto index :
           library::SortedQuicklinkIndices(data_.quicklinks)) {
        const auto& link = data_.quicklinks[index];
        std::wstring detail = link.target;
        const auto keyword = library::NormalizeKeyword(link.keyword);
        const auto duplicates = std::count_if(
            data_.quicklinks.begin(), data_.quicklinks.end(),
            [&](const auto& item) {
              return library::NormalizeKeyword(item.keyword) == keyword;
            });
        if (duplicates > 1) detail = L"[Duplicate keyword] " + detail;
        InsertRow(row++, index, link.name.empty() ? link.keyword : link.name,
                  link.keyword, detail);
      }
    } else if (kind_ == library::ItemKind::AppAlias) {
      int row = 0;
      for (const auto index :
           library::SortedAppAliasIndices(data_.appAliases)) {
        const auto& alias = data_.appAliases[index];
        InsertRow(row++, index,
                  alias.appName.empty() ? L"Missing app" : alias.appName,
                  alias.alias, alias.appId);
      }
    } else {
      int row = 0;
      for (const auto index :
           library::SortedWebSearchIndices(data_.webSearches)) {
        const auto& search = data_.webSearches[index];
        InsertRow(row++, index, search.keyword, search.keyword,
                  search.urlTemplate);
      }
    }
    const auto selected = SelectedIndex();
    EnableWindow(add_, Writable());
    EnableWindow(edit_, Writable() && selected.has_value());
    EnableWindow(remove_, Writable() && selected.has_value());
    EnableWindow(openFile_, kind_ == library::ItemKind::Snippet ||
                                (kind_ == library::ItemKind::WebSearch &&
                                 Writable()));
    SetWindowTextW(openFile_, kind_ == library::ItemKind::WebSearch
                                  ? L"Restore Defaults" : L"Open File");
    const std::wstring message = kind_ == library::ItemKind::Snippet
        ? data_.snippetsMessage
        : (kind_ == library::ItemKind::Quicklink ? data_.quicklinksMessage
                                                  : data_.settingsMessage);
    SetWindowTextW(status_,
                   message.empty()
                       ? (Writable() ? L"Changes are saved immediately."
                                     : L"Editing is unavailable.")
                       : message.c_str());
  }

  void ShowResult(const library::OperationResult& result) {
    if (!result.succeeded) {
      MessageBoxW(hwnd_, result.message.c_str(), L"FeatherCast Library",
                  MB_OK | MB_ICONWARNING);
    }
  }

  void AddItem() {
    if (!Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, data_.quicklinks,
                        data_.appAliases, data_.availableApps,
                        data_.webSearches, std::nullopt, initialAppId_);
    if (!editor.Run()) return;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.push_back(editor.Snippet());
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate.push_back(editor.Quicklink());
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.push_back(editor.Alias());
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate.push_back(editor.WebSearch());
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  void EditItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, data_.quicklinks,
                        data_.appAliases, data_.availableApps,
                        data_.webSearches, selected);
    if (!editor.Run()) return;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate[*selected] = editor.Snippet();
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate[*selected] = editor.Quicklink();
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate[*selected] = editor.Alias();
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate[*selected] = editor.WebSearch();
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  void DeleteItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    if (MessageBoxW(hwnd_, L"Delete the selected library item?",
                    L"FeatherCast Library",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
      return;
    }
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
      case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
      case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IdTabs && header->code == TCN_SELCHANGE) {
          kind_ = static_cast<library::ItemKind>(TabCtrl_GetCurSel(tabs_));
          initialAppId_.clear();
          Refresh();
          return 0;
        }
        if (header->idFrom == IdList && header->code == NM_DBLCLK) {
          EditItem();
          return 0;
        }
        if (header->idFrom == IdList && header->code == LVN_ITEMCHANGED) {
          const bool selected = SelectedIndex().has_value();
          EnableWindow(edit_, Writable() && selected);
          EnableWindow(remove_, Writable() && selected);
        }
        break;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IdAdd: AddItem(); return 0;
          case IdEdit: EditItem(); return 0;
          case IdDelete: DeleteItem(); return 0;
          case IdReload:
            if (callbacks_.reload) data_ = callbacks_.reload();
            Refresh();
            return 0;
          case IdOpenFile:
            if (kind_ == library::ItemKind::WebSearch &&
                callbacks_.restoreDefaultWebSearches) {
              const auto result = callbacks_.restoreDefaultWebSearches();
              ShowResult(result);
              if (callbacks_.reload) data_ = callbacks_.reload();
              Refresh();
            } else if (callbacks_.openSnippetsFile) {
              callbacks_.openSnippetsFile();
            }
            return 0;
          case IDCANCEL:
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
      case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
  }

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  ManagerData data_;
  ManagerCallbacks callbacks_;
  library::ItemKind kind_ = library::ItemKind::Snippet;
  std::wstring initialAppId_;
  HWND tabs_ = nullptr;
  HWND list_ = nullptr;
  HWND add_ = nullptr;
  HWND edit_ = nullptr;
  HWND remove_ = nullptr;
  HWND reload_ = nullptr;
  HWND openFile_ = nullptr;
  HWND close_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace

void ShowLibraryManager(HWND owner, ManagerData data,
                        ManagerCallbacks callbacks,
                        library::ItemKind initialKind,
                        std::wstring initialAppId) {
  ManagerWindow(owner, std::move(data), std::move(callbacks), initialKind,
                std::move(initialAppId)).Run();
}

}  // namespace feathercast::library_ui
