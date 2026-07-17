#pragma once

#include "settings.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace feathercast::settings_io {

struct LoadResult {
  feathercast::settings::Settings value;
  feathercast::settings::ParseStatus status =
      feathercast::settings::ParseStatus::Missing;
  std::filesystem::path preservedPath;
  bool persistenceAllowed = true;
  std::wstring message;
};

inline bool SaveSettingsFile(
    const std::filesystem::path& settingsPath,
    const feathercast::settings::Settings& settings,
    std::wstring* errorMessage = nullptr) {
  std::error_code ec;
  std::filesystem::create_directories(settingsPath.parent_path(), ec);
  if (ec) {
    if (errorMessage) {
      *errorMessage = L"Could not create the settings directory.";
    }
    return false;
  }

  std::filesystem::path temporaryPath = settingsPath;
  temporaryPath += L".tmp";
  {
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (errorMessage) *errorMessage = L"Could not open the temporary settings file.";
      return false;
    }
    const auto serialized =
        feathercast::settings::SerializeSettings(settings);
    file.write(serialized.data(),
               static_cast<std::streamsize>(serialized.size()));
    file.flush();
    if (!file) {
      file.close();
      std::filesystem::remove(temporaryPath, ec);
      if (errorMessage) *errorMessage = L"Could not finish writing settings.";
      return false;
    }
  }

  if (!MoveFileExW(temporaryPath.c_str(), settingsPath.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporaryPath, ec);
    if (errorMessage) *errorMessage = L"Could not replace settings.json.";
    return false;
  }
  return true;
}

inline std::filesystem::path InvalidBackupPath(
    const std::filesystem::path& settingsPath) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  std::filesystem::path backup = settingsPath;
  backup += L".invalid-" + std::to_wstring(timestamp);
  return backup;
}

inline LoadResult LoadSettingsFile(const std::filesystem::path& settingsPath) {
  LoadResult result;
  std::error_code ec;
  if (!std::filesystem::exists(settingsPath, ec)) {
    result.status = feathercast::settings::ParseStatus::Missing;
    return result;
  }
  if (ec) {
    result.status = feathercast::settings::ParseStatus::Invalid;
    result.persistenceAllowed = false;
    result.message = L"FeatherCast could not inspect settings.json. "
                     L"Automatic settings saves are disabled.";
    return result;
  }

  std::ifstream file(settingsPath, std::ios::binary);
  if (!file) {
    result.status = feathercast::settings::ParseStatus::Invalid;
    result.persistenceAllowed = false;
    result.message = L"FeatherCast could not read settings.json. "
                     L"Automatic settings saves are disabled.";
    return result;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.good() && !file.eof()) {
    result.status = feathercast::settings::ParseStatus::Invalid;
    result.persistenceAllowed = false;
    result.message = L"FeatherCast could not finish reading settings.json. "
                     L"Automatic settings saves are disabled.";
    return result;
  }

  const auto parsed = feathercast::settings::ParseSettingsDocument(buffer.str());
  result.value = parsed.value;
  result.status =
      parsed.status == feathercast::settings::ParseStatus::Missing
          ? feathercast::settings::ParseStatus::Invalid
          : parsed.status;
  if (result.status ==
      feathercast::settings::ParseStatus::UnsupportedVersion) {
    result.persistenceAllowed = false;
    result.message =
        L"settings.json was written by a newer FeatherCast version "
        L"(schema " +
        std::to_wstring(parsed.documentVersion) +
        L"). Defaults are active and automatic settings saves are disabled "
        L"to preserve the file.";
    return result;
  }
  if (result.status != feathercast::settings::ParseStatus::Invalid) return result;

  const auto backup = InvalidBackupPath(settingsPath);
  std::filesystem::rename(settingsPath, backup, ec);
  if (!ec) {
    result.preservedPath = backup;
    result.message =
        L"FeatherCast found invalid settings and preserved the original file at " +
        backup.wstring() + L". Defaults are active.";
    return result;
  }

  ec.clear();
  std::filesystem::copy_file(settingsPath, backup,
                             std::filesystem::copy_options::none, ec);
  if (!ec) {
    result.preservedPath = backup;
    result.message =
        L"FeatherCast found invalid settings and preserved a copy at " +
        backup.wstring() + L". Defaults are active.";
    return result;
  }

  result.persistenceAllowed = false;
  result.message =
      L"FeatherCast found invalid settings but could not preserve settings.json. "
      L"Automatic settings saves are disabled to protect the original file.";
  return result;
}

}  // namespace feathercast::settings_io
