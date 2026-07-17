#pragma once

#include "app_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace feathercast::system_settings {

inline app::AppEntry Entry(std::wstring id, std::wstring name,
                           std::wstring uri,
                           std::vector<std::wstring> keywords) {
  app::AppEntry entry;
  entry.id = L"windows-settings:" + std::move(id);
  entry.name = std::move(name);
  entry.source = L"windows-settings";
  entry.launchType = app::LaunchType::Shell;
  entry.launchTarget = std::move(uri);
  entry.systemEssential = true;
  entry.keywords = std::move(keywords);
  return entry;
}

inline std::vector<app::AppEntry> Catalog() {
  return {
      Entry(L"display", L"Display Settings", L"ms-settings:display",
            {L"monitor", L"screen", L"resolution", L"hdr"}),
      Entry(L"sound", L"Sound Settings", L"ms-settings:sound",
            {L"audio", L"volume", L"microphone", L"speaker"}),
      Entry(L"bluetooth", L"Bluetooth & Devices", L"ms-settings:bluetooth",
            {L"devices", L"wireless", L"pair"}),
      Entry(L"installed-apps", L"Installed Apps", L"ms-settings:appsfeatures",
            {L"applications", L"uninstall", L"programs"}),
      Entry(L"windows-update", L"Windows Update", L"ms-settings:windowsupdate",
            {L"updates", L"security", L"patches"}),
  };
}

}  // namespace feathercast::system_settings
