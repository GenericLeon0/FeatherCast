#pragma once

#include "app_types.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace feathercast::runtime {

enum class BackgroundSubsystem {
  Launch,
  Persistence,
  Discovery,
  Search,
  Snapshot,
  Update,
  Currency,
  Icon,
};

struct BackgroundTaskFailed {
  BackgroundSubsystem subsystem = BackgroundSubsystem::Search;
  std::wstring message;
};

struct SettingsSaved {
  bool succeeded = false;
  std::wstring error;
};

struct StorageOperationCompleted {
  app::StorageOperationResult result;
};

struct SearchResultsReady {
  app::ResultsCollection result;
};

struct SearchSnapshotReady {
  app::SnapshotBuildResult result;
};

struct DiscoveryCompleted {
  std::uint64_t generation = 0;
  std::vector<app::AppEntry> apps;
  std::vector<app::AppEntry> fileIndex;
};

struct IconResolved {
  std::wstring key;
};

using UiEvent =
    std::variant<BackgroundTaskFailed, SettingsSaved,
                 StorageOperationCompleted, SearchResultsReady,
                 SearchSnapshotReady, DiscoveryCompleted, IconResolved>;

}  // namespace feathercast::runtime
