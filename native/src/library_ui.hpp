#pragma once

#include "library.hpp"

#include <windows.h>

#include <functional>
#include <vector>

namespace feathercast::library_ui {

struct ManagerData {
  std::vector<snippets::Snippet> snippets;
  std::vector<settings::Quicklink> quicklinks;
  std::vector<library::AppAlias> appAliases;
  std::vector<library::AppChoice> availableApps;
  std::vector<library::WebSearch> webSearches;
  bool snippetsWritable = true;
  bool quicklinksWritable = true;
  bool settingsWritable = true;
  std::wstring snippetsMessage;
  std::wstring quicklinksMessage;
  std::wstring settingsMessage;
};

struct ManagerCallbacks {
  std::function<library::OperationResult(
      const std::vector<snippets::Snippet>&)> saveSnippets;
  std::function<library::OperationResult(
      const std::vector<settings::Quicklink>&)> saveQuicklinks;
  std::function<library::OperationResult(
      const std::vector<library::AppAlias>&)> saveAppAliases;
  std::function<library::OperationResult(
      const std::vector<library::WebSearch>&)> saveWebSearches;
  std::function<library::OperationResult()> restoreDefaultWebSearches;
  std::function<ManagerData()> reload;
  std::function<void()> openSnippetsFile;
};

void ShowLibraryManager(HWND owner, ManagerData data,
                        ManagerCallbacks callbacks,
                        library::ItemKind initialKind,
                        std::wstring initialAppId = {});

}  // namespace feathercast::library_ui
