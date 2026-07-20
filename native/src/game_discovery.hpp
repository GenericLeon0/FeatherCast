#pragma once

#include "app_types.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace feathercast::games {

struct UninstallRecord {
  std::wstring registryKeyName;
  std::wstring displayName;
  std::wstring publisher;
  std::wstring installLocation;
  std::wstring displayIcon;
  std::wstring uninstallString;
};

struct UbisoftInstall {
  std::wstring id;
  std::wstring installDirectory;
};

// Injectable local sources keep provider parsing deterministic in tests. The
// production reader fills these exclusively from local files and registry
// state; no account or network data is involved.
struct DiscoverySources {
  std::filesystem::path steamRoot;
  std::filesystem::path epicManifestDirectory;
  std::filesystem::path battleNetProductDb;
  std::filesystem::path battleNetExecutable;
  std::filesystem::path gogGalaxyExecutable;
  std::vector<UninstallRecord> uninstallRecords;
  std::vector<UbisoftInstall> ubisoftInstalls;
  std::vector<std::filesystem::path> xboxGameRoots;
};

DiscoverySources ReadLocalSources();

std::vector<app::AppEntry> DiscoverFromSources(
    const DiscoverySources& sources,
    const std::vector<app::AppEntry>& shellApps,
    std::stop_token stopToken = {});

std::vector<app::AppEntry> DiscoverInstalledGames(
    const std::vector<app::AppEntry>& shellApps,
    std::stop_token stopToken = {});

}  // namespace feathercast::games
