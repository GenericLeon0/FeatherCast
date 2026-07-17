#pragma once

// Settings model plus JSON (de)serialization, extracted from main.cpp so the
// round-trip can be unit-tested. Clamping of overlayWidth/maxResults stays at
// the call sites in main.cpp next to the MIN/MAX constants.

#include <map>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "extension_protocol.hpp"  // Utf8ToWide / WideToUtf8
#include "json.hpp"

namespace feathercast::settings {

inline constexpr int kCurrentSettingsSchemaVersion = 1;

// A user-defined keyword that opens a URL, file, or folder directly.
struct Quicklink {
  std::wstring keyword;
  std::wstring name;
  std::wstring target;
};

struct PrivacySettings {
  bool clipboardEnabled = false;
  bool fileIndexEnabled = false;
  std::vector<std::wstring> roots;
  int retention = 50;
};

struct Settings {
  std::wstring shortcut = L"Alt+Space";
  std::vector<std::wstring> recentApps;
  std::vector<std::wstring> pinnedApps;
  std::vector<std::wstring> hiddenApps;
  std::map<std::wstring, std::wstring> appAliases;
  struct UsageStat {
    int launches = 0;
    long long lastUsed = 0;
  };
  std::map<std::wstring, UsageStat> usageStats;
  bool compactMode = false;
  bool animationsEnabled = true;
  bool syncAccentColor = true;
  std::wstring customAccentColor = L"#5b6cff";
  bool startOnStartup = false;
  bool updateChecksEnabled = true;
  long long lastUpdateAttempt = 0;
  long long lastUpdateCheck = 0;
  std::wstring dismissedUpdateVersion;
  int overlayWidth = 720;  // WIN_WIDTH in main.cpp
  int maxResults = 200;    // MAX_RESULTS in main.cpp
  bool showOpenWindows = true;
  bool showStoreApps = true;
  // Personal-data features are opt-in. privacyConsentVersion records that the
  // user has seen the disclosure, regardless of which features they enabled.
  int privacyConsentVersion = 0;
  bool clipboardHistoryEnabled = false;
  int clipboardHistoryLimit = 50;
  bool fileIndexEnabled = false;
  int fileIndexMaxEntries = 5000;
  std::vector<std::wstring> fileIndexRoots;
  bool diagnosticsEnabled = false;
  // Web search prefixes: keyword -> URL template containing "%s" for the query.
  std::map<std::wstring, std::wstring> searchEngines = {
    {L"g", L"https://www.google.com/search?q=%s"},
    {L"ddg", L"https://duckduckgo.com/?q=%s"},
    {L"yt", L"https://www.youtube.com/results?search_query=%s"},
    {L"gh", L"https://github.com/search?q=%s"},
    {L"w", L"https://en.wikipedia.org/w/index.php?search=%s"},
  };
  std::vector<Quicklink> quicklinks;

  PrivacySettings Privacy() const {
    return {
      clipboardHistoryEnabled,
      fileIndexEnabled,
      fileIndexRoots,
      clipboardHistoryLimit,
    };
  }
};

enum class ParseStatus {
  Missing,
  Valid,
  Invalid,
  UnsupportedVersion,
};

struct ParseResult {
  Settings value;
  ParseStatus status = ParseStatus::Missing;
  int documentVersion = 0;
};

inline std::string JsonEscape(const std::wstring& value) {
  const std::string in = feathercast::extensions::WideToUtf8(value);
  std::string out;
  for (const char ch : in) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      char buf[8]{};
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
      out += buf;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

namespace detail {

using feathercast::extensions::Utf8ToWide;
using feathercast::json::Value;

inline bool IsValidUtf8(std::string_view text) {
  const auto continuation = [](unsigned char ch) {
    return ch >= 0x80 && ch <= 0xBF;
  };

  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first <= 0x7F) {
      ++i;
      continue;
    }

    if (first >= 0xC2 && first <= 0xDF) {
      if (i + 1 >= text.size() ||
          !continuation(static_cast<unsigned char>(text[i + 1]))) {
        return false;
      }
      i += 2;
      continue;
    }

    if (first >= 0xE0 && first <= 0xEF) {
      if (i + 2 >= text.size()) return false;
      const auto second = static_cast<unsigned char>(text[i + 1]);
      const auto third = static_cast<unsigned char>(text[i + 2]);
      bool validSecond = continuation(second);
      if (first == 0xE0) validSecond = second >= 0xA0 && second <= 0xBF;
      else if (first == 0xED) validSecond = second >= 0x80 && second <= 0x9F;
      if (!validSecond || !continuation(third)) return false;
      i += 3;
      continue;
    }

    if (first >= 0xF0 && first <= 0xF4) {
      if (i + 3 >= text.size()) return false;
      const auto second = static_cast<unsigned char>(text[i + 1]);
      const auto third = static_cast<unsigned char>(text[i + 2]);
      const auto fourth = static_cast<unsigned char>(text[i + 3]);
      bool validSecond = continuation(second);
      if (first == 0xF0) validSecond = second >= 0x90 && second <= 0xBF;
      else if (first == 0xF4) validSecond = second >= 0x80 && second <= 0x8F;
      if (!validSecond || !continuation(third) || !continuation(fourth)) {
        return false;
      }
      i += 4;
      continue;
    }

    return false;
  }
  return true;
}

inline void ReadString(const Value& root, std::string_view key, std::wstring& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::String) {
    out = Utf8ToWide(value->str);
  }
}

inline void ReadBool(const Value& root, std::string_view key, bool& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Bool) {
    out = value->boolean;
  }
}

inline void ReadInt(const Value& root, std::string_view key, int& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Number) {
    if (std::isfinite(value->number) &&
        value->number >= static_cast<double>(std::numeric_limits<int>::min()) &&
        value->number <= static_cast<double>(std::numeric_limits<int>::max())) {
      out = static_cast<int>(value->number);
    }
  }
}

inline void ReadLongLong(const Value& root, std::string_view key, long long& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Number) {
    if (std::isfinite(value->number) &&
        value->number >= static_cast<double>(std::numeric_limits<long long>::min()) &&
        value->number <= static_cast<double>(std::numeric_limits<long long>::max())) {
      out = static_cast<long long>(value->number);
    }
  }
}

inline std::vector<std::wstring> ReadStringArray(const Value& root, std::string_view key) {
  std::vector<std::wstring> out;
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Array) {
    for (const auto& element : value->array) {
      if (element.type == Value::Type::String) out.push_back(Utf8ToWide(element.str));
    }
  }
  return out;
}

inline std::map<std::wstring, std::wstring> ReadStringObject(const Value& root, std::string_view key) {
  std::map<std::wstring, std::wstring> out;
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Object) {
    for (const auto& member : value->object) {
      if (member.value.type == Value::Type::String) {
        out[Utf8ToWide(member.key)] = Utf8ToWide(member.value.str);
      }
    }
  }
  return out;
}

// Missing or malformed fields keep their defaults (same lenient behavior as
// the previous scanner, but keys inside string values can no longer match).
inline Settings ParseSettingsRoot(const std::optional<Value>& root) {
  Settings settings;
  if (!root || root->type != Value::Type::Object) return settings;

  ReadString(*root, "shortcut", settings.shortcut);
  settings.recentApps = ReadStringArray(*root, "recentApps");
  settings.pinnedApps = ReadStringArray(*root, "pinnedApps");
  settings.hiddenApps = ReadStringArray(*root, "hiddenApps");
  settings.appAliases = ReadStringObject(*root, "appAliases");
  if (const Value* stats = root->Find("usageStats"); stats && stats->type == Value::Type::Object) {
    for (const auto& member : stats->object) {
      if (member.value.type != Value::Type::Object) continue;
      Settings::UsageStat stat;
      ReadInt(member.value, "launches", stat.launches);
      ReadLongLong(member.value, "lastUsed", stat.lastUsed);
      settings.usageStats[Utf8ToWide(member.key)] = stat;
    }
  }
  ReadBool(*root, "compactMode", settings.compactMode);
  ReadBool(*root, "animationsEnabled", settings.animationsEnabled);
  ReadBool(*root, "syncAccentColor", settings.syncAccentColor);
  ReadString(*root, "customAccentColor", settings.customAccentColor);
  ReadBool(*root, "startOnStartup", settings.startOnStartup);
  ReadBool(*root, "updateChecksEnabled", settings.updateChecksEnabled);
  ReadLongLong(*root, "lastUpdateAttempt", settings.lastUpdateAttempt);
  ReadLongLong(*root, "lastUpdateCheck", settings.lastUpdateCheck);
  ReadString(*root, "dismissedUpdateVersion", settings.dismissedUpdateVersion);
  ReadInt(*root, "overlayWidth", settings.overlayWidth);
  ReadInt(*root, "maxResults", settings.maxResults);
  ReadBool(*root, "showOpenWindows", settings.showOpenWindows);
  ReadBool(*root, "showStoreApps", settings.showStoreApps);
  ReadInt(*root, "privacyConsentVersion", settings.privacyConsentVersion);
  ReadBool(*root, "clipboardHistoryEnabled", settings.clipboardHistoryEnabled);
  ReadInt(*root, "clipboardHistoryLimit", settings.clipboardHistoryLimit);
  ReadBool(*root, "fileIndexEnabled", settings.fileIndexEnabled);
  ReadInt(*root, "fileIndexMaxEntries", settings.fileIndexMaxEntries);
  settings.fileIndexRoots = ReadStringArray(*root, "fileIndexRoots");
  ReadBool(*root, "diagnosticsEnabled", settings.diagnosticsEnabled);
  if (auto engines = ReadStringObject(*root, "searchEngines"); !engines.empty()) {
    settings.searchEngines = std::move(engines);
  }
  if (const Value* links = root->Find("quicklinks"); links && links->type == Value::Type::Array) {
    for (const auto& element : links->array) {
      if (element.type != Value::Type::Object) continue;
      Quicklink link;
      ReadString(element, "keyword", link.keyword);
      ReadString(element, "name", link.name);
      ReadString(element, "target", link.target);
      if (!link.keyword.empty() && !link.target.empty()) settings.quicklinks.push_back(std::move(link));
    }
  }
  return settings;
}

}  // namespace detail

inline ParseResult ParseSettingsDocument(const std::string& text) {
  if (text.empty()) return {};
  if (!detail::IsValidUtf8(text)) {
    return {Settings{}, ParseStatus::Invalid, 0};
  }

  const auto root = feathercast::json::Parse(text);
  if (!root || root->type != feathercast::json::Value::Type::Object) {
    return {Settings{}, ParseStatus::Invalid, 0};
  }

  int documentVersion = 0;
  if (const auto* version = root->Find("schemaVersion")) {
    if (version->type != feathercast::json::Value::Type::Number ||
        !std::isfinite(version->number) ||
        std::floor(version->number) != version->number ||
        version->number < 0 ||
        version->number >
            static_cast<double>(std::numeric_limits<int>::max())) {
      return {Settings{}, ParseStatus::Invalid, 0};
    }
    documentVersion = static_cast<int>(version->number);
  }

  if (documentVersion > kCurrentSettingsSchemaVersion) {
    return {Settings{}, ParseStatus::UnsupportedVersion, documentVersion};
  }
  return {detail::ParseSettingsRoot(root), ParseStatus::Valid,
          documentVersion};
}

// Compatibility wrapper for callers that intentionally want defaults for a
// missing or invalid document.
inline Settings ParseSettings(const std::string& text) {
  return ParseSettingsDocument(text).value;
}

namespace detail {

inline void WriteStringArray(std::ostringstream& out, const std::vector<std::wstring>& values) {
  out << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << JsonEscape(values[i]) << "\"";
  }
  out << "]";
}

inline void WriteStringObject(std::ostringstream& out, const std::map<std::wstring, std::wstring>& values) {
  out << "{";
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) out << ", ";
    first = false;
    out << "\"" << JsonEscape(key) << "\": \"" << JsonEscape(value) << "\"";
  }
  out << "}";
}

}  // namespace detail

inline std::string SerializeSettings(const Settings& settings) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schemaVersion\": " << kCurrentSettingsSchemaVersion << ",\n";
  out << "  \"shortcut\": \"" << JsonEscape(settings.shortcut) << "\",\n";
  out << "  \"recentApps\": ";
  detail::WriteStringArray(out, settings.recentApps);
  out << ",\n";
  out << "  \"pinnedApps\": ";
  detail::WriteStringArray(out, settings.pinnedApps);
  out << ",\n";
  out << "  \"hiddenApps\": ";
  detail::WriteStringArray(out, settings.hiddenApps);
  out << ",\n";
  out << "  \"appAliases\": ";
  detail::WriteStringObject(out, settings.appAliases);
  out << ",\n";
  out << "  \"usageStats\": {";
  {
    bool first = true;
    for (const auto& [key, stat] : settings.usageStats) {
      if (!first) out << ", ";
      first = false;
      out << "\"" << JsonEscape(key) << "\": {\"launches\": " << stat.launches
          << ", \"lastUsed\": " << stat.lastUsed << "}";
    }
  }
  out << "},\n";
  out << "  \"compactMode\": " << (settings.compactMode ? "true" : "false") << ",\n";
  out << "  \"animationsEnabled\": " << (settings.animationsEnabled ? "true" : "false") << ",\n";
  out << "  \"syncAccentColor\": " << (settings.syncAccentColor ? "true" : "false") << ",\n";
  out << "  \"customAccentColor\": \"" << JsonEscape(settings.customAccentColor) << "\",\n";
  out << "  \"startOnStartup\": " << (settings.startOnStartup ? "true" : "false") << ",\n";
  out << "  \"updateChecksEnabled\": " << (settings.updateChecksEnabled ? "true" : "false") << ",\n";
  out << "  \"lastUpdateAttempt\": " << settings.lastUpdateAttempt << ",\n";
  out << "  \"lastUpdateCheck\": " << settings.lastUpdateCheck << ",\n";
  out << "  \"dismissedUpdateVersion\": \"" << JsonEscape(settings.dismissedUpdateVersion) << "\",\n";
  out << "  \"overlayWidth\": " << settings.overlayWidth << ",\n";
  out << "  \"maxResults\": " << settings.maxResults << ",\n";
  out << "  \"showOpenWindows\": " << (settings.showOpenWindows ? "true" : "false") << ",\n";
  out << "  \"showStoreApps\": " << (settings.showStoreApps ? "true" : "false") << ",\n";
  out << "  \"privacyConsentVersion\": " << settings.privacyConsentVersion << ",\n";
  out << "  \"clipboardHistoryEnabled\": " << (settings.clipboardHistoryEnabled ? "true" : "false") << ",\n";
  out << "  \"clipboardHistoryLimit\": " << settings.clipboardHistoryLimit << ",\n";
  out << "  \"fileIndexEnabled\": " << (settings.fileIndexEnabled ? "true" : "false") << ",\n";
  out << "  \"fileIndexMaxEntries\": " << settings.fileIndexMaxEntries << ",\n";
  out << "  \"fileIndexRoots\": ";
  detail::WriteStringArray(out, settings.fileIndexRoots);
  out << ",\n";
  out << "  \"diagnosticsEnabled\": " << (settings.diagnosticsEnabled ? "true" : "false") << ",\n";
  out << "  \"searchEngines\": ";
  detail::WriteStringObject(out, settings.searchEngines);
  out << ",\n";
  out << "  \"quicklinks\": [";
  {
    bool first = true;
    for (const auto& link : settings.quicklinks) {
      if (!first) out << ", ";
      first = false;
      out << "{\"keyword\": \"" << JsonEscape(link.keyword) << "\", \"name\": \""
          << JsonEscape(link.name) << "\", \"target\": \"" << JsonEscape(link.target) << "\"}";
    }
  }
  out << "]\n";
  out << "}\n";
  return out.str();
}

}  // namespace feathercast::settings
