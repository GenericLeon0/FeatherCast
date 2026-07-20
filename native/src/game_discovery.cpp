#include "game_discovery.hpp"

#include "core.hpp"
#include "discovery.hpp"
#include "extension_protocol.hpp"
#include "json.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::games {
namespace {

using app::AppEntry;
using app::LaunchType;
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxMetadataBytes = 8 * 1024 * 1024;

std::optional<std::string> ReadText(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > kMaxMetadataBytes) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool IsDirectory(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_directory(path, ec);
}

bool IsFile(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec);
}

std::wstring TrimQuotes(std::wstring value) {
  value = core::Trim(value);
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::wstring ExecutableFromCommand(std::wstring value) {
  value = core::Trim(value);
  if (value.empty()) return {};
  if (value.front() == L'"') {
    const auto close = value.find(L'"', 1);
    return close == std::wstring::npos ? std::wstring{}
                                       : value.substr(1, close - 1);
  }
  const auto comma = value.find(L',');
  if (comma != std::wstring::npos) value.resize(comma);
  const auto exe = core::Lower(value).find(L".exe");
  if (exe != std::wstring::npos) value.resize(exe + 4);
  return core::Trim(value);
}

std::wstring ArgumentsFromCommand(std::wstring value) {
  value = core::Trim(value);
  if (value.empty()) return {};
  size_t end = std::wstring::npos;
  if (value.front() == L'"') {
    end = value.find(L'"', 1);
    if (end != std::wstring::npos) ++end;
  } else {
    const auto exe = core::Lower(value).find(L".exe");
    if (exe != std::wstring::npos) end = exe + 4;
  }
  return end == std::wstring::npos ? std::wstring{}
                                   : core::Trim(value.substr(end));
}

std::wstring ProviderKey(std::wstring_view provider) {
  std::wstring key = core::Lower(std::wstring(provider));
  key.erase(std::remove_if(key.begin(), key.end(), [](wchar_t ch) {
              return !std::iswalnum(ch);
            }),
            key.end());
  return key;
}

AppEntry Game(std::wstring provider, std::wstring id, std::wstring name,
              const fs::path& installDirectory) {
  AppEntry entry;
  entry.id = L"game:" + ProviderKey(provider) + L":" + id;
  entry.name = discovery::CleanName(name);
  entry.path = installDirectory.wstring();
  entry.source = L"game";
  entry.isGame = true;
  entry.gameProvider = std::move(provider);
  entry.keywords = discovery::UniqueKeywords(
      {entry.name, entry.gameProvider, L"game", L"games"});
  return entry;
}

std::wstring VdfValue(const std::wstring& text, std::wstring_view key) {
  const std::wregex expression(
      L"\\\"" + std::wstring(key) + L"\\\"\\s*\\\"([^\\\"]*)\\\"",
      std::regex_constants::icase);
  std::wsmatch match;
  if (!std::regex_search(text, match, expression) || match.size() < 2) return {};
  std::wstring value = match[1].str();
  for (size_t pos = 0; (pos = value.find(L"\\\\", pos)) != std::wstring::npos;) {
    value.replace(pos, 2, L"\\");
    ++pos;
  }
  return value;
}

std::vector<fs::path> SteamLibraries(const fs::path& steamRoot) {
  std::vector<fs::path> roots;
  if (IsDirectory(steamRoot / L"steamapps")) roots.push_back(steamRoot);
  const auto text = ReadText(steamRoot / L"steamapps" / L"libraryfolders.vdf");
  if (!text) return roots;
  const std::wstring wide = extensions::Utf8ToWide(*text);
  const std::wregex pathExpression(L"\\\"path\\\"\\s*\\\"([^\\\"]+)\\\"",
                                   std::regex_constants::icase);
  for (std::wsregex_iterator it(wide.begin(), wide.end(), pathExpression), end;
       it != end; ++it) {
    std::wstring value = (*it)[1].str();
    for (size_t pos = 0; (pos = value.find(L"\\\\", pos)) != std::wstring::npos;) {
      value.replace(pos, 2, L"\\");
      ++pos;
    }
    fs::path root(value);
    if (IsDirectory(root / L"steamapps") &&
        std::find(roots.begin(), roots.end(), root) == roots.end()) {
      roots.push_back(std::move(root));
    }
  }
  return roots;
}

bool IsSteamSupportPackage(std::wstring_view name,
                           std::wstring_view installName) {
  const std::wstring value =
      core::Lower(std::wstring(name) + L" " + std::wstring(installName));
  static constexpr std::array<std::wstring_view, 10> markers{
      L"steamworks common", L"steamworks shared", L"steam linux runtime",
      L"proton - ", L"proton easyanticheat", L"proton battleye",
      L"dedicated server", L"source sdk", L"redistributable",
      L"steamvr",
  };
  return std::any_of(markers.begin(), markers.end(), [&](std::wstring_view marker) {
    return value.find(marker) != std::wstring::npos;
  });
}

fs::path SteamIconPath(const fs::path& steamRoot, std::wstring_view appId) {
  const fs::path cacheRoot =
      steamRoot / L"appcache" / L"librarycache";
  const fs::path appCache = cacheRoot / appId;
  for (const auto& candidate : {
           cacheRoot / (std::wstring(appId) + L"_icon.jpg"),
           cacheRoot / (std::wstring(appId) + L"_icon.png"),
           appCache / L"icon.jpg",
           appCache / L"icon.png",
       }) {
    if (IsFile(candidate)) return candidate;
  }

  std::error_code ec;
  for (fs::directory_iterator it(appCache,
                                 fs::directory_options::skip_permission_denied,
                                 ec),
       end;
       it != end; it.increment(ec)) {
    if (ec || !it->is_regular_file(ec)) continue;
    const std::wstring extension =
        core::Lower(it->path().extension().wstring());
    const std::wstring stem = it->path().stem().wstring();
    const bool contentHash =
        stem.size() == 40 &&
        std::all_of(stem.begin(), stem.end(), [](wchar_t ch) {
          return std::iswxdigit(ch) != 0;
        });
    if (contentHash && (extension == L".jpg" || extension == L".jpeg" ||
                        extension == L".png")) {
      return it->path();
    }
  }
  return {};
}

void DiscoverSteam(const DiscoverySources& sources, std::vector<AppEntry>& out,
                   std::stop_token token) {
  const fs::path steamExe = sources.steamRoot / L"steam.exe";
  if (!IsFile(steamExe)) return;
  for (const auto& library : SteamLibraries(sources.steamRoot)) {
    std::error_code ec;
    for (fs::directory_iterator it(library / L"steamapps",
                                   fs::directory_options::skip_permission_denied,
                                   ec),
         end;
         it != end && !token.stop_requested(); it.increment(ec)) {
      if (ec || !it->is_regular_file(ec)) continue;
      const auto fileName = core::Lower(it->path().filename().wstring());
      if (!fileName.starts_with(L"appmanifest_") ||
          core::Lower(it->path().extension().wstring()) != L".acf") {
        continue;
      }
      const auto text = ReadText(it->path());
      if (!text) continue;
      const std::wstring wide = extensions::Utf8ToWide(*text);
      const std::wstring appId = VdfValue(wide, L"appid");
      const std::wstring name = VdfValue(wide, L"name");
      const std::wstring installName = VdfValue(wide, L"installdir");
      const std::wstring stateFlags = VdfValue(wide, L"StateFlags");
      unsigned long flags = 0;
      try {
        flags = std::stoul(stateFlags);
      } catch (...) {
        continue;
      }
      if (appId.empty() || name.empty() || installName.empty() ||
          (flags & 4u) == 0 || IsSteamSupportPackage(name, installName)) {
        continue;
      }
      const fs::path installDir = library / L"steamapps" / L"common" / installName;
      if (!IsDirectory(installDir)) continue;
      auto game = Game(L"Steam", appId, name, installDir);
      game.launchType = LaunchType::Exe;
      game.launchTarget = steamExe.wstring();
      game.args = L"-silent \"steam://rungameid/" + appId + L"\"";
      game.cwd = sources.steamRoot.wstring();
      game.iconKey = SteamIconPath(sources.steamRoot, appId).wstring();
      out.push_back(std::move(game));
    }
  }
}

const json::Value* JsonMember(const json::Value& object, const char* name,
                              json::Value::Type type) {
  const auto* value = object.Find(name);
  return value && value->type == type ? value : nullptr;
}

std::wstring JsonString(const json::Value& object, const char* name) {
  const auto* value = JsonMember(object, name, json::Value::Type::String);
  return value ? extensions::Utf8ToWide(value->str) : std::wstring{};
}

bool JsonBool(const json::Value& object, const char* name, bool fallback) {
  const auto* value = JsonMember(object, name, json::Value::Type::Bool);
  return value ? value->boolean : fallback;
}

bool JsonArrayContains(const json::Value& object, const char* name,
                       std::string_view expected) {
  const auto* value = JsonMember(object, name, json::Value::Type::Array);
  if (!value) return false;
  for (const auto& item : value->array) {
    if (item.type != json::Value::Type::String) continue;
    std::string lower = item.str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (lower == expected) return true;
  }
  return false;
}

void DiscoverEpic(const DiscoverySources& sources, std::vector<AppEntry>& out,
                  std::stop_token token) {
  if (!IsDirectory(sources.epicManifestDirectory)) return;
  std::set<std::wstring> seen;
  std::error_code ec;
  for (fs::directory_iterator it(sources.epicManifestDirectory,
                                 fs::directory_options::skip_permission_denied,
                                 ec),
       end;
       it != end && !token.stop_requested(); it.increment(ec)) {
    if (ec || !it->is_regular_file(ec) ||
        core::Lower(it->path().extension().wstring()) != L".item") {
      continue;
    }
    const auto text = ReadText(it->path());
    const auto root = text ? json::Parse(*text) : std::nullopt;
    if (!root || root->type != json::Value::Type::Object) continue;
    if (!JsonBool(*root, "bIsApplication", false) ||
        !JsonBool(*root, "bIsExecutable", false) ||
        JsonBool(*root, "bIsIncompleteInstall", true) ||
        !JsonArrayContains(*root, "AppCategories", "games")) {
      continue;
    }
    const std::wstring appName = JsonString(*root, "AppName");
    const std::wstring displayName = JsonString(*root, "DisplayName");
    const fs::path installDir(JsonString(*root, "InstallLocation"));
    const fs::path launchExe = installDir / JsonString(*root, "LaunchExecutable");
    if (appName.empty() || displayName.empty() || !IsDirectory(installDir) ||
        !IsFile(launchExe) || !seen.insert(core::Lower(appName)).second) {
      continue;
    }
    auto game = Game(L"Epic Games", appName, displayName, installDir);
    game.launchType = LaunchType::Shell;
    game.launchTarget = L"com.epicgames.launcher://apps/" + appName +
                        L"?action=launch&silent=true";
    game.targetPath = launchExe.wstring();
    game.iconKey = launchExe.wstring();
    out.push_back(std::move(game));
  }
}

std::optional<AppEntry> GogGame(const UninstallRecord& record,
                                const fs::path& galaxyExe) {
  static const std::wregex keyPattern(LR"(^([0-9]+)_is1$)",
                                      std::regex_constants::icase);
  std::wsmatch match;
  if (!std::regex_match(record.registryKeyName, match, keyPattern) ||
      core::Lower(record.publisher) != L"gog.com") {
    return std::nullopt;
  }
  const fs::path installDir(record.installLocation);
  if (!IsDirectory(installDir)) return std::nullopt;
  const std::wstring gameId = match[1].str();
  auto game = Game(L"GOG", gameId, record.displayName, installDir);

  const auto manifestText = ReadText(installDir / (L"goggame-" + gameId + L".info"));
  const auto manifest = manifestText ? json::Parse(*manifestText) : std::nullopt;
  if (manifest && manifest->type == json::Value::Type::Object) {
    if (const auto* tasks = JsonMember(*manifest, "playTasks", json::Value::Type::Array)) {
      for (const auto& task : tasks->array) {
        if (task.type != json::Value::Type::Object ||
            !JsonBool(task, "isPrimary", false)) {
          continue;
        }
        const std::wstring pathText = JsonString(task, "path");
        fs::path executable(pathText);
        if (executable.is_relative()) executable = installDir / executable;
        if (!IsFile(executable)) continue;
        game.launchType = LaunchType::Exe;
        game.launchTarget = executable.wstring();
        game.targetPath = executable.wstring();
        game.args = JsonString(task, "arguments");
        fs::path cwd(JsonString(task, "workingDir"));
        if (cwd.empty()) cwd = installDir;
        if (cwd.is_relative()) cwd = installDir / cwd;
        game.cwd = cwd.wstring();
        game.iconKey = executable.wstring();
        game.adminSupported = true;
        return game;
      }
    }
  }

  // The local game manifest is also our proof that this uninstall entry is a
  // runnable base game rather than DLC, bonus content, or a stale registry key.
  if (!manifest || manifest->type != json::Value::Type::Object ||
      !IsFile(galaxyExe)) {
    return std::nullopt;
  }
  game.launchType = LaunchType::Exe;
  game.launchTarget = galaxyExe.wstring();
  game.args = L"/launchViaAutostart /gameId=" + gameId +
              L" /command=runGame /path=\"" + installDir.wstring() + L"\"";
  game.cwd = galaxyExe.parent_path().wstring();
  game.iconKey = record.displayIcon.empty() ? galaxyExe.wstring()
                                            : ExecutableFromCommand(record.displayIcon);
  return game;
}

void DiscoverGog(const DiscoverySources& sources, std::vector<AppEntry>& out,
                 std::stop_token token) {
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    if (auto game = GogGame(record, sources.gogGalaxyExecutable)) {
      out.push_back(std::move(*game));
    }
  }
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view part) {
  return core::Lower(std::wstring(value)).find(core::Lower(std::wstring(part))) !=
         std::wstring::npos;
}

void DiscoverEa(const DiscoverySources& sources, std::vector<AppEntry>& out,
                std::stop_token token) {
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    if (!ContainsInsensitive(record.publisher, L"electronic arts") ||
        core::Lower(record.displayName) == L"ea app") {
      continue;
    }
    const fs::path installDir(record.installLocation);
    const fs::path executable(ExecutableFromCommand(record.displayIcon));
    if (!IsDirectory(installDir) || !IsFile(executable) ||
        !ContainsInsensitive(executable.extension().wstring(), L".exe") ||
        ContainsInsensitive(executable.filename().wstring(), L"uninstall") ||
        ContainsInsensitive(executable.filename().wstring(), L"cleanup")) {
      continue;
    }
    auto game = Game(L"EA app", record.registryKeyName, record.displayName,
                     installDir);
    game.launchType = LaunchType::Exe;
    game.launchTarget = executable.wstring();
    game.targetPath = executable.wstring();
    game.cwd = installDir.wstring();
    game.iconKey = executable.wstring();
    game.adminSupported = true;
    out.push_back(std::move(game));
  }
}

void DiscoverUbisoft(const DiscoverySources& sources, std::vector<AppEntry>& out,
                     std::stop_token token) {
  for (const auto& install : sources.ubisoftInstalls) {
    if (token.stop_requested()) return;
    const fs::path installDir(install.installDirectory);
    if (install.id.empty() || !IsDirectory(installDir)) continue;
    std::wstring name = installDir.filename().wstring();
    if (name.empty()) name = L"Ubisoft game " + install.id;
    auto game = Game(L"Ubisoft Connect", install.id, name, installDir);
    game.launchType = LaunchType::Shell;
    game.launchTarget = L"uplay://launch/" + install.id;
    out.push_back(std::move(game));
  }
}

std::wstring BattleUid(const std::wstring& uninstallString) {
  const std::wregex pattern(LR"(--uid=([^\s\"]+))",
                            std::regex_constants::icase);
  std::wsmatch match;
  return std::regex_search(uninstallString, match, pattern) ? match[1].str()
                                                            : std::wstring{};
}

void AddBattleGame(std::vector<AppEntry>& out, std::set<std::wstring>& ids,
                   const fs::path& battleExe, std::wstring uid,
                   std::wstring name, const fs::path& installDir) {
  if (uid.empty() || !IsDirectory(installDir) || !IsFile(battleExe) ||
      !ids.insert(core::Lower(uid)).second) {
    return;
  }
  if (name.empty()) name = installDir.filename().wstring();
  auto game = Game(L"Battle.net", uid, name, installDir);
  game.launchType = LaunchType::Exe;
  game.launchTarget = battleExe.wstring();
  game.args = L"--exec=\"launch " + uid + L"\"";
  game.cwd = battleExe.parent_path().wstring();
  game.iconKey = battleExe.wstring();
  out.push_back(std::move(game));
}

bool ReadVarint(std::span<const unsigned char> data, size_t& pos,
                std::uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && pos < data.size(); shift += 7) {
    const unsigned char byte = data[pos++];
    value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

struct ProtoField {
  unsigned number = 0;
  std::span<const unsigned char> bytes;
};

std::vector<ProtoField> LengthFields(std::span<const unsigned char> data) {
  std::vector<ProtoField> fields;
  size_t pos = 0;
  while (pos < data.size()) {
    std::uint64_t tag = 0;
    if (!ReadVarint(data, pos, tag) || tag == 0) break;
    const unsigned wire = static_cast<unsigned>(tag & 7);
    const unsigned number = static_cast<unsigned>(tag >> 3);
    if (wire == 2) {
      std::uint64_t length = 0;
      if (!ReadVarint(data, pos, length) || length > data.size() - pos) break;
      fields.push_back({number, data.subspan(pos, static_cast<size_t>(length))});
      pos += static_cast<size_t>(length);
    } else if (wire == 0) {
      std::uint64_t ignored = 0;
      if (!ReadVarint(data, pos, ignored)) break;
    } else if (wire == 1) {
      if (data.size() - pos < 8) break;
      pos += 8;
    } else if (wire == 5) {
      if (data.size() - pos < 4) break;
      pos += 4;
    } else {
      break;
    }
  }
  return fields;
}

std::wstring ProtoString(std::span<const unsigned char> bytes) {
  return extensions::Utf8ToWide(std::string(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::optional<std::wstring> BattleProductName(std::wstring_view code) {
  static const std::map<std::wstring, std::wstring> names{
      {L"d3", L"Diablo III"},
      {L"d4", L"Diablo IV"},
      {L"fen", L"Diablo II: Resurrected"},
      {L"hero", L"Heroes of the Storm"},
      {L"pro", L"Overwatch"},
      {L"s1", L"StarCraft"},
      {L"s2", L"StarCraft II"},
      {L"wtcg", L"Hearthstone"},
      {L"wow", L"World of Warcraft"},
      {L"wow_classic", L"World of Warcraft Classic"},
      {L"wow_classic_era", L"World of Warcraft Classic Era"},
      {L"odin", L"Call of Duty: Modern Warfare"},
      {L"lazr", L"Call of Duty: Modern Warfare II"},
      {L"zeus", L"Call of Duty: Black Ops Cold War"},
      {L"auks", L"Call of Duty: Black Ops 4"},
  };
  const auto found = names.find(core::Lower(std::wstring(code)));
  return found == names.end() ? std::nullopt
                              : std::optional<std::wstring>(found->second);
}

void DiscoverBattleNet(const DiscoverySources& sources,
                       std::vector<AppEntry>& out, std::stop_token token) {
  std::set<std::wstring> ids;
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    const std::wstring uid = BattleUid(record.uninstallString);
    if (uid.empty() || !ContainsInsensitive(record.uninstallString, L"battle.net")) {
      continue;
    }
    AddBattleGame(out, ids, sources.battleNetExecutable, uid,
                  record.displayName, record.installLocation);
  }

  std::ifstream input(sources.battleNetProductDb, std::ios::binary);
  if (!input || token.stop_requested()) return;
  const std::vector<char> rawBytes{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  const std::vector<unsigned char> bytes(rawBytes.begin(), rawBytes.end());
  if (bytes.size() > kMaxMetadataBytes) return;
  for (const auto& wrapper : LengthFields(bytes)) {
    if (token.stop_requested()) return;
    std::wstring uid;
    std::wstring productCode;
    fs::path installDir;
    for (const auto& field : LengthFields(wrapper.bytes)) {
      if (field.number == 1) uid = ProtoString(field.bytes);
      if (field.number == 2) productCode = ProtoString(field.bytes);
      if (field.number == 3) {
        const fs::path directPath(ProtoString(field.bytes));
        if (IsDirectory(directPath)) installDir = directPath;
        for (const auto& nested : LengthFields(field.bytes)) {
          if (nested.number == 1) installDir = ProtoString(nested.bytes);
        }
      }
    }
    auto name = BattleProductName(productCode);
    std::wstring launchId = productCode;
    if (!name) {
      name = BattleProductName(uid);
      launchId = uid;
    }
    if (!name) continue;
    AddBattleGame(out, ids, sources.battleNetExecutable, launchId, *name,
                  installDir);
  }
}

std::wstring XmlAttribute(const std::wstring& text, std::wstring_view element,
                          std::wstring_view attribute) {
  const std::wregex expression(
      L"<\\s*" + std::wstring(element) + L"\\b[^>]*\\b" +
          std::wstring(attribute) + L"\\s*=\\s*\\\"([^\\\"]+)\\\"",
      std::regex_constants::icase);
  std::wsmatch match;
  return std::regex_search(text, match, expression) ? match[1].str()
                                                    : std::wstring{};
}

void DiscoverXbox(const DiscoverySources& sources,
                  const std::vector<AppEntry>& shellApps,
                  std::vector<AppEntry>& out, std::stop_token token) {
  std::set<std::wstring> matchedAumids;
  for (const auto& root : sources.xboxGameRoots) {
    if (!IsDirectory(root)) continue;
    std::error_code ec;
    for (fs::directory_iterator it(root,
                                   fs::directory_options::skip_permission_denied,
                                   ec),
         end;
         it != end && !token.stop_requested(); it.increment(ec)) {
      if (ec || !it->is_directory(ec)) continue;
      const fs::path content = it->path() / L"Content";
      const fs::path config = content / L"MicrosoftGame.config";
      const auto xmlBytes = ReadText(config);
      if (!xmlBytes) continue;
      const std::wstring xml = extensions::Utf8ToWide(*xmlBytes);
      const std::wstring identity = XmlAttribute(xml, L"Identity", L"Name");
      std::wstring name = XmlAttribute(xml, L"ShellVisuals", L"DefaultDisplayName");
      const std::wstring executable = XmlAttribute(xml, L"Executable", L"Name");
      const fs::path executablePath = content / executable;
      if (identity.empty() || executable.empty() || !IsFile(executablePath)) {
        continue;
      }
      if (name.empty() || name.starts_with(L"ms-resource:")) {
        name = it->path().filename().wstring();
      }
      const std::wstring lowerIdentity = core::Lower(identity);
      const std::wstring lowerName = core::Lower(name);
      const auto shell = std::find_if(shellApps.begin(), shellApps.end(),
                                      [&](const AppEntry& app) {
        if (app.appUserModelId.empty()) return false;
        return core::Lower(app.name) == lowerName ||
               core::Lower(app.appUserModelId).find(lowerIdentity) !=
                   std::wstring::npos;
      });
      if (shell == shellApps.end() ||
          !matchedAumids.insert(core::Lower(shell->appUserModelId)).second) {
        continue;
      }
      AppEntry game = *shell;
      game.id = L"game:xbox:" + identity;
      game.name = name;
      game.path = it->path().wstring();
      game.source = L"game";
      game.isGame = true;
      game.gameProvider = L"Xbox";
      game.targetPath = executablePath.wstring();
      game.keywords = discovery::UniqueKeywords(
          {name, identity, L"Xbox", L"Microsoft Store", L"game", L"games"});
      out.push_back(std::move(game));
    }
  }
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name, nullptr, &type,
                      reinterpret_cast<BYTE*>(value.data()), &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  if (type == REG_EXPAND_SZ && !value.empty()) {
    const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed > 1) {
      std::wstring expanded(needed, L'\0');
      ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
      while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
      value = std::move(expanded);
    }
  }
  return value;
}

std::wstring ReadRegistryPath(HKEY root, const wchar_t* subkey,
                              const wchar_t* value, REGSAM view = 0) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return {};
  }
  const std::wstring result = ReadRegistryString(raw, value);
  RegCloseKey(raw);
  return result;
}

void ReadUninstallView(HKEY root, REGSAM view,
                       std::vector<UninstallRecord>& out) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return;
  }
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 512> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (RegEnumKeyExW(raw, index, name.data(), &length, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS) {
      break;
    }
    HKEY child = nullptr;
    if (RegOpenKeyExW(raw, name.data(), 0, KEY_READ | view, &child) != ERROR_SUCCESS) {
      continue;
    }
    UninstallRecord record;
    record.registryKeyName.assign(name.data(), length);
    record.displayName = ReadRegistryString(child, L"DisplayName");
    record.publisher = ReadRegistryString(child, L"Publisher");
    record.installLocation = ReadRegistryString(child, L"InstallLocation");
    record.displayIcon = ReadRegistryString(child, L"DisplayIcon");
    record.uninstallString = ReadRegistryString(child, L"UninstallString");
    RegCloseKey(child);
    if (!record.displayName.empty()) out.push_back(std::move(record));
  }
  RegCloseKey(raw);
}

void ReadUbisoftView(REGSAM view, std::vector<UbisoftInstall>& out) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Ubisoft\\Launcher\\Installs", 0,
                    KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return;
  }
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 256> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (RegEnumKeyExW(raw, index, name.data(), &length, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS) {
      break;
    }
    HKEY child = nullptr;
    if (RegOpenKeyExW(raw, name.data(), 0, KEY_READ | view, &child) != ERROR_SUCCESS) {
      continue;
    }
    UbisoftInstall install;
    install.id.assign(name.data(), length);
    install.installDirectory = ReadRegistryString(child, L"InstallDir");
    RegCloseKey(child);
    if (!install.installDirectory.empty()) out.push_back(std::move(install));
  }
  RegCloseKey(raw);
}

std::vector<fs::path> XboxRoots() {
  std::vector<fs::path> roots;
  const DWORD drives = GetLogicalDrives();
  for (int index = 0; index < 26; ++index) {
    if ((drives & (1u << index)) == 0) continue;
    const fs::path drive(std::wstring(1, static_cast<wchar_t>(L'A' + index)) + L":\\");
    const fs::path conventional = drive / L"XboxGames";
    if (IsDirectory(conventional)) roots.push_back(conventional);

    std::ifstream file(drive / L".GamingRoot", std::ios::binary);
    if (!file) continue;
    const std::vector<char> rawBytes{std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>()};
    const std::vector<unsigned char> bytes(rawBytes.begin(), rawBytes.end());
    for (size_t offset : {size_t{6}, size_t{8}}) {
      if (offset >= bytes.size()) continue;
      std::wstring folder;
      for (size_t pos = offset; pos + 1 < bytes.size(); pos += 2) {
        const wchar_t ch = static_cast<wchar_t>(bytes[pos] | (bytes[pos + 1] << 8));
        if (ch == L'\0') break;
        if (ch < 0x20) {
          folder.clear();
          break;
        }
        folder.push_back(ch);
      }
      const fs::path candidate = drive / folder;
      if (!folder.empty() && IsDirectory(candidate) &&
          std::find(roots.begin(), roots.end(), candidate) == roots.end()) {
        roots.push_back(candidate);
      }
    }
  }
  return roots;
}

}  // namespace

DiscoverySources ReadLocalSources() {
  DiscoverySources sources;
  std::wstring steam = ReadRegistryPath(HKEY_CURRENT_USER,
                                        L"Software\\Valve\\Steam", L"SteamPath");
  if (steam.empty()) {
    steam = ReadRegistryPath(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam",
                             L"InstallPath", KEY_WOW64_32KEY);
  }
  std::replace(steam.begin(), steam.end(), L'/', L'\\');
  sources.steamRoot = steam;

  std::array<wchar_t, 32768> programData{};
  if (GetEnvironmentVariableW(L"ProgramData", programData.data(),
                              static_cast<DWORD>(programData.size())) > 0) {
    sources.epicManifestDirectory =
        fs::path(programData.data()) / L"Epic" / L"EpicGamesLauncher" /
        L"Data" / L"Manifests";
    sources.battleNetProductDb = fs::path(programData.data()) / L"Battle.net" /
                                 L"Agent" / L"product.db";
  }

  ReadUninstallView(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_CURRENT_USER, KEY_WOW64_64KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_CURRENT_USER, KEY_WOW64_32KEY,
                    sources.uninstallRecords);
  ReadUbisoftView(KEY_WOW64_32KEY, sources.ubisoftInstalls);
  ReadUbisoftView(KEY_WOW64_64KEY, sources.ubisoftInstalls);

  for (const auto& record : sources.uninstallRecords) {
    const std::wstring lowerName = core::Lower(record.displayName);
    if (lowerName == L"battle.net") {
      fs::path executable(ExecutableFromCommand(record.displayIcon));
      if (!IsFile(executable)) executable = fs::path(record.installLocation) / L"Battle.net.exe";
      if (IsFile(executable)) sources.battleNetExecutable = executable;
    }
    if (lowerName == L"gog galaxy") {
      fs::path executable = fs::path(record.installLocation) / L"GalaxyClient.exe";
      if (IsFile(executable)) sources.gogGalaxyExecutable = executable;
    }
  }
  sources.xboxGameRoots = XboxRoots();
  return sources;
}

std::vector<AppEntry> DiscoverFromSources(
    const DiscoverySources& sources, const std::vector<AppEntry>& shellApps,
    std::stop_token stopToken) {
  std::vector<AppEntry> games;
  DiscoverSteam(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverEpic(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverGog(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverEa(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverUbisoft(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverBattleNet(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverXbox(sources, shellApps, games, stopToken);
  return games;
}

std::vector<AppEntry> DiscoverInstalledGames(
    const std::vector<AppEntry>& shellApps, std::stop_token stopToken) {
  return DiscoverFromSources(ReadLocalSources(), shellApps, stopToken);
}

}  // namespace feathercast::games
