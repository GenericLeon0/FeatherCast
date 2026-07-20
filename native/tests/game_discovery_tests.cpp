#ifdef NDEBUG
#undef NDEBUG
#endif

#include "game_discovery.hpp"
#include "search_pipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using feathercast::app::AppEntry;

void Write(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream output(path, std::ios::binary);
  assert(output.good());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  assert(output.good());
}

void Touch(const fs::path& path) { Write(path, "fixture"); }

void Varint(std::vector<unsigned char>& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<unsigned char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<unsigned char>(value));
}

void Field(std::vector<unsigned char>& out, unsigned number,
           const std::vector<unsigned char>& value) {
  Varint(out, (static_cast<std::uint64_t>(number) << 3) | 2);
  Varint(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}

std::vector<unsigned char> Bytes(const std::string& value) {
  return {value.begin(), value.end()};
}

void WriteBytes(const fs::path& path, const std::vector<unsigned char>& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

const AppEntry* FindProvider(const std::vector<AppEntry>& games,
                             const std::wstring& provider,
                             const std::wstring& name = {}) {
  const auto found = std::find_if(games.begin(), games.end(), [&](const AppEntry& game) {
    return game.gameProvider == provider && (name.empty() || game.name == name);
  });
  return found == games.end() ? nullptr : &*found;
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() /
                        (L"FeatherCastGamesTests-" +
                         std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(GetTickCount64()));
  std::error_code ec;
  fs::create_directories(root, ec);
  assert(!ec);

  feathercast::games::DiscoverySources sources;

  sources.steamRoot = root / L"Steam";
  Touch(sources.steamRoot / L"steam.exe");
  const fs::path steamIcon =
      sources.steamRoot / L"appcache" / L"librarycache" / L"42" /
      L"0123456789abcdef0123456789abcdef01234567.jpg";
  Touch(steamIcon);
  Write(sources.steamRoot / L"steamapps" / L"appmanifest_42.acf",
        R"("AppState"
{
  "appid" "42"
  "name" "Steam Fixture"
  "installdir" "SteamFixture"
  "StateFlags" "4"
})");
  fs::create_directories(sources.steamRoot / L"steamapps" / L"common" /
                         L"SteamFixture", ec);
  Write(sources.steamRoot / L"steamapps" / L"appmanifest_44.acf",
        R"("AppState" { "appid" "44" "name" "Incomplete Steam Fixture" "installdir" "Incomplete" "StateFlags" "2" })");
  fs::create_directories(sources.steamRoot / L"steamapps" / L"common" /
                         L"Incomplete", ec);
  Write(sources.steamRoot / L"steamapps" / L"appmanifest_45.acf",
        R"("AppState" { "appid" "45" "name" "Steamworks Common Redistributables" "installdir" "Steamworks Shared" "StateFlags" "4" })");
  fs::create_directories(sources.steamRoot / L"steamapps" / L"common" /
                         L"Steamworks Shared", ec);
  const fs::path secondSteamLibrary = root / L"SteamLibrary";
  Write(sources.steamRoot / L"steamapps" / L"libraryfolders.vdf",
        "\"libraryfolders\" { \"1\" { \"path\" \"" +
            feathercast::extensions::WideToUtf8(
                secondSteamLibrary.generic_wstring()) +
            "\" } }");
  Write(secondSteamLibrary / L"steamapps" / L"appmanifest_43.acf",
        R"("AppState" { "appid" "43" "name" "Second Steam Fixture" "installdir" "SecondFixture" "StateFlags" "4" })");
  fs::create_directories(secondSteamLibrary / L"steamapps" / L"common" /
                         L"SecondFixture", ec);

  sources.epicManifestDirectory = root / L"Epic" / L"Manifests";
  const fs::path epicInstall = root / L"EpicGame";
  Touch(epicInstall / L"Game.exe");
  Write(sources.epicManifestDirectory / L"valid.item",
        "{\"AppName\":\"epic-fixture\",\"DisplayName\":\"Epic Fixture\","
        "\"InstallLocation\":\"" +
            feathercast::extensions::WideToUtf8(epicInstall.generic_wstring()) +
            "\",\"LaunchExecutable\":\"Game.exe\","
            "\"AppCategories\":[\"games\",\"applications\"],"
            "\"bIsApplication\":true,\"bIsExecutable\":true,"
            "\"bIsIncompleteInstall\":false}");
  Write(sources.epicManifestDirectory / L"incomplete.item",
        R"({"AppName":"bad","DisplayName":"Bad","InstallLocation":"C:\\bad","LaunchExecutable":"bad.exe","AppCategories":["games"],"bIsApplication":true,"bIsExecutable":true,"bIsIncompleteInstall":true})");
  Write(sources.epicManifestDirectory / L"malformed.item", "{");

  const fs::path gogInstall = root / L"GogGame";
  Touch(gogInstall / L"GogGame.exe");
  Write(gogInstall / L"goggame-100.info",
        R"({"playTasks":[{"isPrimary":true,"path":"GogGame.exe","arguments":"-play","workingDir":"."}]})");
  sources.uninstallRecords.push_back(
      {L"100_is1", L"GOG Fixture", L"GOG.com", gogInstall.wstring(), L"", L""});
  sources.uninstallRecords.push_back(
      {L"101_is1", L"GOG DLC Fixture", L"GOG.com", gogInstall.wstring(), L"", L""});

  const fs::path eaInstall = root / L"EaGame";
  Touch(eaInstall / L"EaGame.exe");
  sources.uninstallRecords.push_back(
      {L"{EA-FIXTURE}", L"EA Fixture", L"Electronic Arts, Inc.",
       eaInstall.wstring(), L"\"" + (eaInstall / L"EaGame.exe").wstring() +
                                L"\",0",
       L""});
  sources.uninstallRecords.push_back(
      {L"{EA-APP}", L"EA app", L"Electronic Arts", L"", L"", L""});

  const fs::path ubiInstall = root / L"Ubisoft Fixture";
  fs::create_directories(ubiInstall, ec);
  sources.ubisoftInstalls.push_back({L"777", ubiInstall.wstring()});

  sources.battleNetExecutable = root / L"Battle.net" / L"Battle.net.exe";
  Touch(sources.battleNetExecutable);
  const fs::path battleInstall = root / L"Battle Fixture";
  fs::create_directories(battleInstall, ec);
  sources.uninstallRecords.push_back(
      {L"battle", L"Battle Fixture", L"Blizzard Entertainment",
       battleInstall.wstring(), L"", L"Battle.net.exe --uid=fixture "});

  const fs::path productInstall = root / L"Product Fixture";
  fs::create_directories(productInstall, ec);
  std::vector<unsigned char> product;
  Field(product, 1, Bytes("product-fixture"));
  Field(product, 2, Bytes("D3"));
  Field(product, 3,
        Bytes(feathercast::extensions::WideToUtf8(productInstall.wstring())));
  std::vector<unsigned char> unknownProduct;
  Field(unknownProduct, 1, Bytes("unknown-fixture"));
  Field(unknownProduct, 2, Bytes("UNKNOWN"));
  Field(unknownProduct, 3,
        Bytes(feathercast::extensions::WideToUtf8(productInstall.wstring())));
  std::vector<unsigned char> database;
  Field(database, 1, product);
  Field(database, 1, unknownProduct);
  sources.battleNetProductDb = root / L"Battle.net" / L"Agent" / L"product.db";
  WriteBytes(sources.battleNetProductDb, database);

  const fs::path xboxRoot = root / L"XboxGames";
  const fs::path xboxContent = xboxRoot / L"Xbox Fixture" / L"Content";
  Touch(xboxContent / L"XboxGame.exe");
  Write(xboxContent / L"MicrosoftGame.config",
        R"(<?xml version="1.0"?><Game><Identity Name="Publisher.XboxFixture"/><ExecutableList><Executable Name="XboxGame.exe" Id="Game"/></ExecutableList><ShellVisuals DefaultDisplayName="Xbox Fixture"/></Game>)");
  sources.xboxGameRoots.push_back(xboxRoot);
  AppEntry xboxShell;
  xboxShell.id = L"start:xbox";
  xboxShell.name = L"Xbox Fixture";
  xboxShell.source = L"appx";
  xboxShell.launchType = feathercast::app::LaunchType::AppsFolder;
  xboxShell.launchTarget = L"Publisher.XboxFixture_hash!Game";
  xboxShell.appUserModelId = xboxShell.launchTarget;

  const auto games = feathercast::games::DiscoverFromSources(sources, {xboxShell});
  if (games.size() != 9) {
    std::wcerr << L"Expected 9 games, found " << games.size() << L":\n";
    for (const auto& game : games) {
      std::wcerr << L"  " << game.gameProvider << L" / " << game.name << L"\n";
    }
  }
  assert(games.size() == 9);
  assert(FindProvider(games, L"Steam", L"Steam Fixture"));
  assert(FindProvider(games, L"Steam", L"Second Steam Fixture"));
  assert(!FindProvider(games, L"Steam", L"Incomplete Steam Fixture"));
  assert(!FindProvider(games, L"Steam", L"Steamworks Common Redistributables"));
  assert(FindProvider(games, L"Epic Games", L"Epic Fixture"));
  assert(FindProvider(games, L"GOG", L"GOG Fixture"));
  assert(FindProvider(games, L"EA app", L"EA Fixture"));
  assert(FindProvider(games, L"Ubisoft Connect", L"Ubisoft Fixture"));
  assert(FindProvider(games, L"Battle.net", L"Battle Fixture"));
  assert(FindProvider(games, L"Battle.net", L"Diablo III"));
  assert(!FindProvider(games, L"Battle.net", L"unknown-fixture"));
  const auto* xbox = FindProvider(games, L"Xbox", L"Xbox Fixture");
  assert(xbox && xbox->launchType == feathercast::app::LaunchType::AppsFolder);

  const auto* steam = FindProvider(games, L"Steam");
  assert(steam && steam->id == L"game:steam:42");
  assert(steam->args == L"-silent \"steam://rungameid/42\"");
  assert(steam->iconKey == steamIcon.wstring());
  const auto* epic = FindProvider(games, L"Epic Games");
  assert(epic && epic->launchTarget ==
                     L"com.epicgames.launcher://apps/epic-fixture?action=launch&silent=true");
  const auto* battleProduct = FindProvider(games, L"Battle.net", L"Diablo III");
  assert(battleProduct && battleProduct->id == L"game:battlenet:D3" &&
         battleProduct->args == L"--exec=\"launch D3\"");

  std::stop_source stopped;
  stopped.request_stop();
  assert(feathercast::games::DiscoverFromSources(sources, {xboxShell},
                                                  stopped.get_token())
             .empty());

  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  feathercast::app::DisplayItem gameItem;
  gameItem.app = *steam;
  snapshot->pool.push_back(gameItem);
  feathercast::core::SearchItem searchItem;
  searchItem.id = steam->id;
  searchItem.name = steam->name;
  searchItem.kind = L"game";
  searchItem.source = L"game";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(searchItem));
  snapshot->gameItems.push_back(gameItem);
  snapshot->gameSearchItems.push_back(searchItem);

  feathercast::app::QueryRequest request;
  request.snapshot = snapshot;
  request.limit = 20;
  request.scope = feathercast::search_scope::Scope::Games;
  request.empty = true;
  auto results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.size() == 1 && results.sections[0].title == L"Games");

  request.scope = feathercast::search_scope::Scope::All;
  request.browseView = feathercast::app::BrowseView::Games;
  request.query = L"steam";
  request.empty = false;
  results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.size() == 1 && results.flatItems[0].app.isGame);

  const auto suggestions = feathercast::search_scope::Suggestions(L"@ga");
  assert(suggestions.size() == 1 &&
         suggestions.front()->scope == feathercast::search_scope::Scope::Games);

  const auto* secondSteam =
      FindProvider(games, L"Steam", L"Second Steam Fixture");
  assert(secondSteam);
  feathercast::app::DisplayItem secondGameItem;
  secondGameItem.app = *secondSteam;
  snapshot->pool.push_back(secondGameItem);
  feathercast::core::SearchItem secondGameSearch;
  secondGameSearch.id = secondSteam->id;
  secondGameSearch.name = secondSteam->name;
  secondGameSearch.kind = L"game";
  secondGameSearch.source = L"game";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(secondGameSearch));

  for (int index = 1; index <= 2; ++index) {
    feathercast::app::DisplayItem appItem;
    appItem.app.id = L"app:" + std::to_wstring(index);
    appItem.app.name = L"Steam Utility " + std::to_wstring(index);
    appItem.app.source = L"shortcut";
    snapshot->pool.push_back(appItem);
    feathercast::core::SearchItem appSearch;
    appSearch.id = appItem.app.id;
    appSearch.name = appItem.app.name;
    appSearch.kind = L"app";
    appSearch.source = L"shortcut";
    snapshot->searchItems.push_back(
        feathercast::core::PrepareSearchItem(appSearch));
  }

  request.browseView = feathercast::app::BrowseView::None;
  request.scope = feathercast::search_scope::Scope::All;
  results = feathercast::search_pipeline::ComputeResults(request);
  const auto gamesSection = std::find_if(
      results.sections.begin(), results.sections.end(),
      [](const auto& section) { return section.title == L"Games"; });
  const auto appsSection = std::find_if(
      results.sections.begin(), results.sections.end(),
      [](const auto& section) { return section.title == L"Apps"; });
  assert(gamesSection != results.sections.end());
  assert(appsSection != results.sections.end());
  assert(gamesSection < appsSection);

  request.snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  request.browseView = feathercast::app::BrowseView::Games;
  request.query.clear();
  request.empty = true;
  results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.empty() && results.sections.empty());

  fs::remove_all(root, ec);
  return 0;
}
