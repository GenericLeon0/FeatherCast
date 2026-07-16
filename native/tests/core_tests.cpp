#include "calculator.hpp"
#include "converter.hpp"
#include "background_executor.hpp"
#include "core.hpp"
#include "discovery.hpp"
#include "emoji.hpp"
#include "extension_protocol.hpp"
#include "json.hpp"
#include "run_command.hpp"
#include "settings.hpp"
#include "shortcut.hpp"
#include "snippets.hpp"
#include "storage.hpp"
#include "symbols.hpp"
#include "theme.hpp"
#include "text_edit.hpp"
#include "updater.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <string_view>

using feathercast::core::ScoreItem;
using feathercast::core::ScoreText;
using feathercast::core::Search;
using feathercast::core::SearchOptions;
using feathercast::core::SearchPrepared;
using feathercast::core::SearchItem;
using feathercast::calculator::TryEvaluate;
using feathercast::converter::TryConvert;
using feathercast::extensions::DiscoverManifests;
using feathercast::extensions::HostActionType;
using feathercast::extensions::LoadManifest;
using feathercast::extensions::ParseActivationResponse;
using feathercast::extensions::ParseManifestJson;
using feathercast::extensions::ParseQueryResponse;
using feathercast::extensions::ResponseSizeAllowed;
using feathercast::snippets::ParseSnippetsJson;
using feathercast::shortcut::ParseShortcut;
using feathercast::shortcut::PressedModifiers;
using feathercast::shortcut::ShortcutRecorder;
using feathercast::shortcut::ShortcutRuntime;
using feathercast::shortcut::ShouldHandleInLowLevelHook;
using feathercast::shortcut::ToHotKeySpec;
using feathercast::updater::CompareVersionStrings;
using feathercast::updater::ExtractSha256Hex;
using feathercast::updater::IsEligibleRelease;
using feathercast::updater::IsNewerVersion;
using feathercast::updater::ParseGitHubReleaseJson;
using feathercast::updater::SelectInstallerAsset;
using feathercast::updater::SelectSha256Asset;
using feathercast::updater::VerifyFileSha256;

namespace {

std::filesystem::path TestTempRoot(std::wstring_view name) {
  return std::filesystem::temp_directory_path() /
         (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()));
}

PressedModifiers Mods(bool ctrl = false, bool alt = false, bool shift = false, bool win = false) {
  return {ctrl, alt, shift, win};
}

void AssertPassOnly(const feathercast::shortcut::HookResult& result) {
  assert(!result.consume);
  assert(!result.toggle);
  assert(!result.suppressWinStart);
}

void AssertRecordingPending(const feathercast::shortcut::RecordingResult& result) {
  assert(result.consume);
  assert(!result.done);
  assert(!result.canceled);
  assert(result.shortcut.empty());
}

void AssertRecordingCanceled(const feathercast::shortcut::RecordingResult& result) {
  assert(result.consume);
  assert(!result.done);
  assert(result.canceled);
  assert(result.shortcut.empty());
}

void AssertRecorded(const feathercast::shortcut::RecordingResult& result, const std::wstring& shortcut) {
  assert(result.consume);
  assert(result.done);
  assert(!result.canceled);
  assert(result.shortcut == shortcut);
}

void WriteUtf8(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

}  // namespace

int main() {
  {
    feathercast::background::Executor executor;
    executor.Start(1);
    std::promise<int> completed;
    auto result = completed.get_future();
    assert(executor.Submit([&](std::stop_token) { completed.set_value(42); }));
    assert(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(result.get() == 42);
    executor.Shutdown();
  }
  {
    feathercast::background::Executor executor;
    executor.Start(1);
    std::atomic<int> completed = 0;
    for (int i = 0; i < 5; ++i) {
      assert(executor.Submit([&](std::stop_token) { completed.fetch_add(1); }));
    }
    executor.Shutdown(true);
    assert(completed.load() == 5);
  }

  assert(ScoreText(L"terminal", L"Terminal") == 5000);
  assert(ScoreText(L"term", L"Terminal") > ScoreText(L"trm", L"Terminal"));
  assert(ScoreText(L"calc", L"Calculator") > 0);
  assert(ScoreText(L"xyz", L"Calculator") < 0);
  assert(ScoreText(L"uber", L"Über") > 0);
  assert(ScoreText(L"cafe", L"Cafe\u0301") > 0);

  {
    std::wstring emojiText = L"A\U0001F600B";
    size_t position = 3;
    assert(feathercast::text_edit::PreviousCodePoint(emojiText, position) == 1);
    assert(feathercast::text_edit::ErasePrevious(emojiText, position));
    assert(emojiText == L"AB");
    assert(position == 1);
  }

  {
    const auto simple = TryEvaluate(L"9+9");
    assert(simple && simple->display == L"18");

    const auto precedence = TryEvaluate(L"2+3*4");
    assert(precedence && precedence->display == L"14");

    const auto grouped = TryEvaluate(L"(2+3)*4");
    assert(grouped && grouped->display == L"20");

    const auto decimal = TryEvaluate(L"1,5+2.25");
    assert(decimal && decimal->display == L"3.75");

    const auto percent = TryEvaluate(L"50%");
    assert(percent && percent->display == L"0.5");

    const auto addPercent = TryEvaluate(L"100 + 10%");
    assert(addPercent && addPercent->display == L"110");

    const auto subtractPercent = TryEvaluate(L"100 - 10%");
    assert(subtractPercent && subtractPercent->display == L"90");

    const auto multiplyPercent = TryEvaluate(L"100 * 10%");
    assert(multiplyPercent && multiplyPercent->display == L"10");

    const auto sine = TryEvaluate(L"sin(90)");
    assert(sine && std::fabs(sine->value - 1.0) < 0.000001);

    const auto cosine = TryEvaluate(L"cos(60)");
    assert(cosine && std::fabs(cosine->value - 0.5) < 0.000001);

    const auto tangent = TryEvaluate(L"tan(45)");
    assert(tangent && std::fabs(tangent->value - 1.0) < 0.000001);

    const auto root = TryEvaluate(L"sqrt(16)");
    assert(root && root->display == L"4");

    const auto power = TryEvaluate(L"2^3");
    assert(power && power->display == L"8");

    const auto powerRightAssoc = TryEvaluate(L"2**3**2");
    assert(powerRightAssoc && powerRightAssoc->display == L"512");

    assert(!TryEvaluate(L"9+"));
    assert(!TryEvaluate(L"notepad"));
  }

  {
    // Unit conversions (offline).
    const auto length = TryConvert(L"10 km to mi");
    assert(length && std::fabs(length->value - 6.21371) < 0.001);

    const auto attached = TryConvert(L"10km to mi");
    assert(attached && std::fabs(attached->value - 6.21371) < 0.001);

    const auto mass = TryConvert(L"1 kg in lb");
    assert(mass && std::fabs(mass->value - 2.20462) < 0.001);

    const auto boiling = TryConvert(L"100 c to f");
    assert(boiling && std::fabs(boiling->value - 212.0) < 0.001);

    const auto kelvin = TryConvert(L"0 c to k");
    assert(kelvin && std::fabs(kelvin->value - 273.15) < 0.001);

    const auto arrow = TryConvert(L"5 km -> m");
    assert(arrow && std::fabs(arrow->value - 5000.0) < 0.001);

    const auto dataRate = TryConvert(L"100 mbps to gbps");
    assert(dataRate && std::fabs(dataRate->value - 0.1) < 0.0001);

    const auto powerUnit = TryConvert(L"1 kw to hp");
    assert(powerUnit && std::fabs(powerUnit->value - 1.34102) < 0.001);

    const auto energy = TryConvert(L"1 kwh to j");
    assert(energy && std::fabs(energy->value - 3600000.0) < 0.001);

    // Cross-category and non-conversions are rejected so the calculator wins.
    assert(!TryConvert(L"10 km to kg"));
    assert(!TryConvert(L"2+3"));
    assert(!TryConvert(L"notepad"));

    // Currency conversion uses an injected rate table (units per 1 USD).
    const std::map<std::wstring, double> rates = {{L"USD", 1.0}, {L"EUR", 0.5}, {L"GBP", 0.8}};
    const auto usdEur = TryConvert(L"100 usd to eur", rates);
    assert(usdEur && std::fabs(usdEur->value - 50.0) < 0.001);

    const auto eurGbp = TryConvert(L"10 eur to gbp", rates);
    assert(eurGbp && std::fabs(eurGbp->value - 16.0) < 0.001);

    // An unknown code paired with a known one is not a unit conversion.
    assert(!TryConvert(L"100 abc to eur", rates));
    // With no rate table, currency codes do not resolve.
    assert(!TryConvert(L"100 usd to eur"));

    // Currency-first amount and "=" connector ("USD 5 = GBP").
    const auto usdGbp = TryConvert(L"USD 5 = GBP", rates);
    assert(usdGbp && std::fabs(usdGbp->value - 4.0) < 0.001);

    // A lone currency converts to the supplied locale currency.
    const auto defaultTarget = TryConvert(L"5 usd", rates, L"EUR");
    assert(defaultTarget && std::fabs(defaultTarget->value - 2.5) < 0.001);

    // Currency-first lone amount also uses the locale currency.
    const auto currencyFirstDefault = TryConvert(L"USD 5", rates, L"EUR");
    assert(currencyFirstDefault && std::fabs(currencyFirstDefault->value - 2.5) < 0.001);

    // Without a locale currency a lone amount is not a conversion.
    assert(!TryConvert(L"5 usd", rates));
    // Same source and locale currency is suppressed (no "5 EUR = 5 EUR").
    assert(!TryConvert(L"5 eur", rates, L"EUR"));

    // Currency symbols expand to their ISO codes ("$5", "EUR5 = $").
    const auto symbolGbp = TryConvert(L"$5 = \u00A3", rates);
    assert(symbolGbp && std::fabs(symbolGbp->value - 4.0) < 0.001);

    const auto symbolDefault = TryConvert(L"$5", rates, L"EUR");
    assert(symbolDefault && std::fabs(symbolDefault->value - 2.5) < 0.001);

    const auto euroSymbol = TryConvert(L"\u20AC5 = $", rates);
    assert(euroSymbol && std::fabs(euroSymbol->value - 10.0) < 0.001);
  }

  {
    assert(CompareVersionStrings(L"0.2.0", L"0.2.1") < 0);
    assert(CompareVersionStrings(L"v1.10.0", L"1.2.9") > 0);
    assert(CompareVersionStrings(L"1.0.0", L"1.0.0") == 0);
    assert(IsNewerVersion(L"0.2.0", L"v0.3.0"));
    assert(!IsNewerVersion(L"0.3.0", L"v0.2.9"));

    const auto release = ParseGitHubReleaseJson(
        "{\"tag_name\":\"v0.3.0\",\"name\":\"FeatherCast 0.3.0\",\"html_url\":\"https://github.com/GenericLeon0/FeatherCast/releases/tag/v0.3.0\","
        "\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"FeatherCast-0.3.0-win64.exe\",\"browser_download_url\":\"https://example.test/FeatherCast-0.3.0-win64.exe\"},"
        "{\"name\":\"FeatherCast-0.3.0-win64.exe.sha256\",\"browser_download_url\":\"https://example.test/FeatherCast-0.3.0-win64.exe.sha256\"}"
        "]}");
    assert(release);
    assert(IsEligibleRelease(*release, L"0.2.0"));
    const auto installer = SelectInstallerAsset(*release);
    assert(installer && installer->name == L"FeatherCast-0.3.0-win64.exe");
    const auto hash = SelectSha256Asset(*release, *installer);
    assert(hash && hash->name == L"FeatherCast-0.3.0-win64.exe.sha256");

    const auto prerelease = ParseGitHubReleaseJson(
        "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":true,\"assets\":[]}");
    assert(prerelease && !IsEligibleRelease(*prerelease, L"0.2.0"));

    const auto extracted = ExtractSha256Hex(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  file.txt");
    assert(extracted && *extracted == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto signerPins = feathercast::updater::ParseSignerThumbprints(
        L"BA:78:16:BF:8F:01:CF:EA:41:41:40:DE:5D:AE:22:23:B0:03:61:A3:96:17:7A:9C:B4:10:FF:61:F2:00:15:AD;"
        L"invalid");
    assert(signerPins.size() == 1);
    assert(signerPins.front() ==
           L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(feathercast::updater::ParseSignerThumbprints(L"").empty());

    const auto tempRoot = TestTempRoot(L"FeatherCastUpdaterCoreTests");
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    WriteUtf8(tempRoot / L"hash.txt", "abc");
    assert(VerifyFileSha256(tempRoot / L"hash.txt",
                            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(!VerifyFileSha256(tempRoot / L"hash.txt",
                             "0000000000000000000000000000000000000000000000000000000000000000"));
    assert(!feathercast::updater::VerifyAuthenticodePublisher(tempRoot / L"hash.txt", L""));
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto tempRoot = TestTempRoot(L"FeatherCastExtensionCoreTests");
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    const auto pluginDir = tempRoot / L"data" / L"plugins" / L"sample";
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"sample\",\"name\":\"Sample\",\"version\":\"1.0\",\"dll\":\"sample.dll\"}");
    const auto manifest = LoadManifest(pluginDir / L"plugin.json");
    assert(manifest.manifest);
    assert(manifest.manifest->id == L"sample");
    assert(manifest.manifest->name == L"Sample");
    assert(manifest.manifest->enabled);
    assert(feathercast::extensions::PathInsideDirectory(manifest.manifest->dllPath, pluginDir));

    const auto missing = ParseManifestJson("{\"id\":\"bad\",\"name\":\"Bad\",\"dll\":\"bad.dll\"}",
                                           pluginDir / L"missing.json");
    assert(!missing.manifest);

    const auto escaping = ParseManifestJson("{\"id\":\"bad/path\",\"name\":\"Bad\",\"version\":\"1\",\"dll\":\"bad.dll\"}",
                                            pluginDir / L"bad-id.json");
    assert(!escaping.manifest);

    const auto outside = ParseManifestJson("{\"id\":\"escape\",\"name\":\"Escape\",\"version\":\"1\",\"dll\":\"..\\\\escape.dll\"}",
                                           pluginDir / L"escape.json");
    assert(!outside.manifest);

    const auto exePluginDir = tempRoot / L"exe" / L"plugins" / L"sample";
    WriteUtf8(exePluginDir / L"plugin.json",
              "{\"id\":\"sample\",\"name\":\"Bundled Sample\",\"version\":\"1.0\",\"dll\":\"bundled.dll\"}");
    const auto discovered = DiscoverManifests(tempRoot / L"data", tempRoot / L"exe");
    assert(discovered.manifests.size() == 1);
    assert(discovered.manifests.front().name == L"Sample");

    assert(ResponseSizeAllowed(1));
    assert(ResponseSizeAllowed(feathercast::extensions::kMaxResponseBytes));
    assert(!ResponseSizeAllowed(0));
    assert(!ResponseSizeAllowed(feathercast::extensions::kMaxResponseBytes + 1));

    const auto query = ParseQueryResponse(
        "{\"items\":[{\"id\":\"one\",\"title\":\"One\",\"subtitle\":\"From plugin\","
        "\"keywords\":[\"uno\",\"first\"],\"score\":42,\"iconPath\":\"C:/icon.png\","
        "\"payload\":{\"answer\":1}}]}");
    assert(query && query->items.size() == 1);
    assert(query->items.front().id == L"one");
    assert(query->items.front().title == L"One");
    assert(query->items.front().subtitle == L"From plugin");
    assert(query->items.front().keywords.size() == 2);
    assert(query->items.front().score == 42.0);
    assert(query->items.front().payloadJson.find("\"answer\"") != std::string::npos);
    assert(!ParseQueryResponse("{}"));

    const auto detailQuery = ParseQueryResponse(
        "{\"items\":[{\"id\":\"detail\",\"title\":\"Detail\",\"detail\":{\"type\":\"markdown\","
        "\"title\":\"Info\",\"body\":\"# Heading\\n- Item\"}}]}");
    assert(detailQuery && detailQuery->items.size() == 1);
    assert(detailQuery->items.front().detailType == L"markdown");
    assert(detailQuery->items.front().detailTitle == L"Info");
    assert(detailQuery->items.front().detailBody.find(L"Heading") != std::wstring::npos);

    const auto activation = ParseActivationResponse(
        "{\"handled\":true,\"closeOverlay\":false,\"action\":{\"type\":\"copyText\",\"value\":\"copied\"}}");
    assert(activation);
    assert(activation->handled);
    assert(!activation->closeOverlay);
    assert(activation->action == HostActionType::CopyText);
    assert(activation->value == L"copied");

    const auto setQuery = ParseActivationResponse(
        "{\"handled\":true,\"closeOverlay\":false,\"action\":{\"type\":\"setQuery\",\"value\":\"follow up\"}}");
    assert(setQuery);
    assert(setQuery->handled);
    assert(!setQuery->closeOverlay);
    assert(setQuery->action == HostActionType::SetQuery);
    assert(setQuery->value == L"follow up");

    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto snippets = ParseSnippetsJson(
        "{\"snippets\":["
        "{\"keyword\":\"sig\",\"name\":\"Email Signature\",\"text\":\"Best,\\nLeon\"},"
        "{\"keyword\":\"\",\"name\":\"Missing Keyword\",\"text\":\"ignored\"},"
        "{\"keyword\":\"bad\",\"name\":\"Missing Text\"},"
        "{\"keyword\":\"empty\",\"name\":\"Empty Text\",\"text\":\"   \"}"
        "]}");
    assert(snippets.size() == 1);
    assert(snippets.front().keyword == L"sig");
    assert(snippets.front().name == L"Email Signature");
    assert(snippets.front().text == L"Best,\nLeon");
  }

  {
    const auto tempRoot = TestTempRoot(L"FeatherCastPhase5StorageTests");
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    feathercast::storage::Storage storage;
    assert(storage.Open(tempRoot / L"feathercast.db"));

    std::vector<feathercast::storage::FileIndexEntry> files = {
      {L"C:\\Users\\Leon\\Documents\\Notes", L"Notes", true, L"C:\\Users\\Leon\\Documents\\Notes", 10, 0, 100},
      {L"C:\\Users\\Leon\\Downloads\\setup.exe", L"setup.exe", false, L"C:\\Users\\Leon\\Downloads\\setup.exe", 11, 42, 100},
    };
    assert(storage.ReplaceFileIndex(files));
    const auto loadedFiles = storage.LoadFileIndex();
    assert(loadedFiles.size() == 2);
    assert(loadedFiles.front().name == L"Notes");
    assert(loadedFiles.front().isDirectory);

    files = {
      {L"C:\\Users\\Leon\\Downloads\\setup.exe", L"setup.exe", false,
       L"C:\\Users\\Leon\\Downloads\\setup.exe", 12, 84, 101},
      {L"C:\\Users\\Leon\\Documents\\todo.txt", L"todo.txt", false,
       L"C:\\Users\\Leon\\Documents\\todo.txt", 13, 21, 101},
    };
    assert(storage.UpdateFileIndex(files));
    const auto updatedFiles = storage.LoadFileIndex();
    assert(updatedFiles.size() == 2);
    assert(updatedFiles.front().name == L"setup.exe");
    assert(updatedFiles.front().size == 84);
    assert(updatedFiles.back().name == L"todo.txt");

    assert(storage.AddClipboardEntry(L"first", L"first", 1, 2));
    assert(storage.AddClipboardEntry(L"second", L"second", 2, 2));
    assert(storage.AddClipboardEntry(L"first", L"first", 3, 2));
    const auto clips = storage.LoadClipboardHistory(10);
    assert(clips.size() == 2);
    assert(clips.front().text == L"first");
    assert(clips.front().capturedAt == 3);
    assert(clips.back().text == L"second");
    assert(storage.ClearClipboardHistory());
    assert(storage.LoadClipboardHistory(10).empty());

    storage.Close();
    WriteUtf8(tempRoot / L"corrupt.db", "not a sqlite database");
    feathercast::storage::Storage recovered;
    assert(recovered.Open(tempRoot / L"corrupt.db"));
    assert(recovered.RecoveredFromCorruption());
    assert(!recovered.QuarantinedPath().empty());
    assert(std::filesystem::exists(recovered.QuarantinedPath()));
    assert(recovered.LoadClipboardHistory().empty());
    recovered.Close();

    const auto legacyPath = tempRoot / L"legacy.db";
    sqlite3* legacyDb = nullptr;
    assert(sqlite3_open16(legacyPath.c_str(), &legacyDb) == SQLITE_OK);
    assert(sqlite3_exec(
               legacyDb,
               "CREATE TABLE clipboard_history ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "text TEXT NOT NULL,"
               "preview TEXT NOT NULL,"
               "captured_at INTEGER NOT NULL);"
               "INSERT INTO clipboard_history(text,preview,captured_at) "
               "VALUES('legacy secret','legacy preview',42);"
               "PRAGMA user_version=1;",
               nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacyDb);

    feathercast::storage::Storage migrated;
    assert(migrated.Open(legacyPath));
    const auto migratedClips = migrated.LoadClipboardHistory();
    assert(migratedClips.size() == 1);
    assert(migratedClips.front().text == L"legacy secret");
    assert(std::filesystem::exists(legacyPath.wstring() + L".pre-v2.bak"));
    migrated.Close();

    assert(sqlite3_open16(legacyPath.c_str(), &legacyDb) == SQLITE_OK);
    sqlite3_stmt* encryptedRow = nullptr;
    assert(sqlite3_prepare_v2(
               legacyDb,
               "SELECT text, encrypted FROM clipboard_history LIMIT 1;",
               -1, &encryptedRow, nullptr) == SQLITE_OK);
    assert(sqlite3_step(encryptedRow) == SQLITE_ROW);
    assert(sqlite3_column_int(encryptedRow, 1) == 1);
    const auto* storedCiphertext =
        static_cast<const wchar_t*>(sqlite3_column_text16(encryptedRow, 0));
    assert(storedCiphertext && std::wstring(storedCiphertext) != L"legacy secret");
    sqlite3_finalize(encryptedRow);
    sqlite3_close(legacyDb);

    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto parsed = feathercast::theme::ParseThemeJson(
        "{\"fontFamily\":\"Cascadia Mono\","
        "\"overlayBackground\":\"#11223380\","
        "\"textPrimary\":\"not-a-color\","
        "\"rowRadius\":12,"
        "\"controlRadius\":500}");
    assert(parsed.fontFamily == L"Cascadia Mono");
    assert(std::fabs(parsed.overlayBackground.r - (0x11 / 255.0f)) < 0.001);
    assert(std::fabs(parsed.overlayBackground.a - (0x80 / 255.0f)) < 0.001);
    assert(std::fabs(parsed.textPrimary.r - feathercast::theme::Theme{}.textPrimary.r) < 0.001);
    assert(parsed.rowRadius == 12.0f);
    assert(parsed.controlRadius == 20.0f);

    const auto color = feathercast::theme::ParseHexColor(L"#ABCDEF");
    assert(color);
    assert(std::fabs(color->g - (0xCD / 255.0f)) < 0.001);
    assert(!feathercast::theme::ParseHexColor(L"#NOPE"));
  }

  {
    const auto url = feathercast::run_command::Classify(L">example.com/path");
    assert(url);
    assert(url->kind == feathercast::run_command::Kind::OpenTarget);
    assert(url->target == L"https://example.com/path");

    const auto scheme = feathercast::run_command::Classify(L">ms-settings:display");
    assert(scheme);
    assert(scheme->kind == feathercast::run_command::Kind::OpenTarget);
    assert(scheme->target == L"ms-settings:display");

    const auto pathRoot = TestTempRoot(L"FeatherCastRunCommandTests");
    std::filesystem::create_directories(pathRoot);
    const auto pathCommand = feathercast::run_command::Classify(L">\"" + pathRoot.wstring() + L"\"");
    assert(pathCommand);
    assert(pathCommand->kind == feathercast::run_command::Kind::OpenTarget);
    assert(pathCommand->target == pathRoot.wstring());
    std::error_code ec;
    std::filesystem::remove_all(pathRoot, ec);

    const auto shell = feathercast::run_command::Classify(L">echo hello");
    assert(shell);
    assert(shell->kind == feathercast::run_command::Kind::ShellCommand);
    assert(shell->input == L"echo hello");
  }

  {
    const auto arrows = feathercast::symbols::SearchSymbols(L":arrow", 5);
    assert(!arrows.empty());
    assert(arrows.front().value == L"\u2192" || arrows.front().label.find(L"Arrow") != std::wstring::npos);

    const auto checks = feathercast::symbols::SearchSymbols(L":check", 5);
    assert(!checks.empty());
    assert(checks.front().label.find(L"Check") != std::wstring::npos);

    const auto smiles = feathercast::symbols::SearchSymbols(L":smile", 5);
    assert(!smiles.empty());
    assert(smiles.front().label.find(L"Face") != std::wstring::npos);
  }

  {
    // The generated emoji table must be present and searchable.
    assert(!feathercast::emoji::AllEmoji().empty());
    for (const auto& emoji : feathercast::emoji::AllEmoji()) {
      assert(!emoji.value.empty());
      assert(!emoji.label.empty());
    }

    const auto empty = feathercast::emoji::SearchEmoji(L"", 10);
    assert(empty.size() == 10);

    const auto smile = feathercast::emoji::SearchEmoji(L"smile", 5);
    assert(!smile.empty());
    assert(smile.front().label.find(L"smil") != std::wstring::npos ||
           smile.front().label.find(L"grin") != std::wstring::npos);

    const auto fire = feathercast::emoji::SearchEmoji(L"fire", 5);
    assert(!fire.empty());
  }

  SearchItem terminal;
  terminal.id = L"terminal";
  terminal.kind = L"app";
  terminal.source = L"alias";
  terminal.name = L"Windows Terminal";
  terminal.keywords = {L"wt", L"shell", L"console", L"powershell"};

  SearchItem notepad;
  notepad.id = L"notepad";
  notepad.kind = L"app";
  notepad.source = L"shortcut";
  notepad.name = L"Notepad";

  SearchItem window;
  window.id = L"hwnd:1";
  window.kind = L"window";
  window.name = L"main.cpp - Visual Studio";
  window.processName = L"devenv";

  SearchItem snippet;
  snippet.id = L"snippet:sig";
  snippet.kind = L"snippet";
  snippet.source = L"snippet";
  snippet.name = L"Email Signature";
  snippet.keywords = {L"sig", L"Best,\nLeon"};

  SearchItem clipboard;
  clipboard.id = L"clipboard:1";
  clipboard.kind = L"clipboard";
  clipboard.source = L"clipboard";
  clipboard.name = L"Clipboard Text";
  clipboard.keywords = {L"recent copied value"};

  std::vector<SearchItem> items = {notepad, terminal, window, snippet, clipboard};
  const auto shell = Search(L"shell", items);
  assert(!shell.empty() && shell.front() == 1);

  const auto acronym = Search(L"wt", items);
  assert(!acronym.empty() && acronym.front() == 1);

  const auto cpp = Search(L"studio", items);
  assert(!cpp.empty() && cpp.front() == 2);
  assert(ScoreText(L"termainl", L"Terminal") > 0);

  const auto snippetHit = Search(L"sig", items);
  assert(!snippetHit.empty() && snippetHit.front() == 3);

  const auto clipboardHit = Search(L"copied value", items);
  assert(!clipboardHit.empty() && clipboardHit.front() == 4);

  {
    std::vector<feathercast::core::PreparedSearchItem> prepared;
    for (const auto& item : items) prepared.push_back(feathercast::core::PrepareSearchItem(item));
    SearchOptions options;
    options.limit = 2;
    const auto limited = SearchPrepared(L"i", prepared, {}, options);
    const auto full = Search(L"i", items);
    assert(limited.size() == std::min<size_t>(2, full.size()));
    assert(std::equal(limited.begin(), limited.end(), full.begin()));

    std::atomic<unsigned long long> latestGeneration = 2;
    options.generation = 1;
    options.latestGeneration = &latestGeneration;
    assert(SearchPrepared(L"terminal", prepared, {}, options).empty());
  }

  const double base = ScoreItem(L"notepad", notepad, {});
  const double recent = ScoreItem(L"notepad", notepad, {L"notepad"});
  assert(recent > base);

  SearchItem plainCode;
  plainCode.id = L"plain-code";
  plainCode.kind = L"app";
  plainCode.name = L"Code";

  SearchItem pinnedCode = plainCode;
  pinnedCode.id = L"pinned-code";
  pinnedCode.pinned = true;
  assert(ScoreItem(L"code", pinnedCode, {}) > ScoreItem(L"code", plainCode, {}));

  SearchItem usedCode = plainCode;
  usedCode.id = L"used-code";
  usedCode.usageCount = 5;
  usedCode.lastUsed = 1000;
  assert(ScoreItem(L"code", usedCode, {}) > ScoreItem(L"code", plainCode, {}));

  SearchItem codeBlocks;
  codeBlocks.id = L"codeblocks";
  codeBlocks.kind = L"app";
  codeBlocks.name = L"CodeBlocks";

  SearchItem visualStudioCode;
  visualStudioCode.id = L"vscode";
  visualStudioCode.kind = L"app";
  visualStudioCode.name = L"Visual Studio Code";
  visualStudioCode.keywords = {L"code"};
  visualStudioCode.usageCount = 20;
  visualStudioCode.lastUsed = 1000;
  assert(ScoreItem(L"code", visualStudioCode, {L"vscode"}) > ScoreItem(L"code", codeBlocks, {}));

  {
    const long long now = 1750000000;

    // A frequently used, recently launched app must beat a cold prefix match
    // even without a matching keyword.
    SearchItem dailyDriver;
    dailyDriver.id = L"vscode-daily";
    dailyDriver.kind = L"app";
    dailyDriver.name = L"Visual Studio Code";
    dailyDriver.usageCount = 30;
    dailyDriver.lastUsed = now;
    assert(ScoreItem(L"code", dailyDriver, {L"vscode-daily"}, now) > ScoreItem(L"code", codeBlocks, {}, now));

    // Recency decays over time.
    SearchItem aged = dailyDriver;
    const double fresh = ScoreItem(L"code", aged, {}, now);
    aged.lastUsed = now - 30LL * 86400;
    const double monthOld = ScoreItem(L"code", aged, {}, now);
    aged.lastUsed = now - 365LL * 86400;
    const double yearOld = ScoreItem(L"code", aged, {}, now);
    assert(fresh > monthOld && monthOld > yearOld);

    // Frequency keeps counting past the old cap of 20, with diminishing returns.
    auto scoreWithUsage = [&](int usage) {
      SearchItem item = dailyDriver;
      item.usageCount = usage;
      return ScoreItem(L"code", item, {}, now);
    };
    assert(scoreWithUsage(100) > scoreWithUsage(20));
    assert(scoreWithUsage(10) - scoreWithUsage(5) > scoreWithUsage(105) - scoreWithUsage(100));

    // Exact-name matches stay dominant over any usage-boosted prefix match.
    SearchItem exactCode;
    exactCode.id = L"exact-code";
    exactCode.kind = L"app";
    exactCode.name = L"Code";
    SearchItem hotPrefix = dailyDriver;
    hotPrefix.name = L"Code Insiders";
    hotPrefix.usageCount = 500;
    assert(ScoreItem(L"code", exactCode, {}, now) > ScoreItem(L"code", hotPrefix, {L"vscode-daily"}, now));

    // camelCase boundaries earn boundary credit.
    assert(ScoreText(L"fb", L"FooBar") > ScoreText(L"fb", L"foobar"));
    assert(ScoreText(L"bar", L"FooBar") > ScoreText(L"bar", L"xxxbar"));
  }

  {
    using feathercast::json::Parse;
    using feathercast::json::Value;

    const auto doc = Parse(R"({"a": [1, -2.5e2, "xy"], "b": {"c": true, "d": null}})");
    assert(doc && doc->type == Value::Type::Object);
    const Value* a = doc->Find("a");
    assert(a && a->type == Value::Type::Array && a->array.size() == 3);
    assert(a->array[0].number == 1 && a->array[1].number == -250);
    assert(a->array[2].str == "xy");
    const Value* b = doc->Find("b");
    assert(b && b->Find("c")->boolean && b->Find("d")->type == Value::Type::Null);

    // \uXXXX escapes decode to UTF-8, including surrogate pairs. The inputs are
    // built with an explicit backslash char to keep this source file pure ASCII.
    const char BS = 0x5C;
    const std::string umlaut = std::string("\"x") + BS + "u00e4\"";      // "xä"
    assert(Parse(umlaut)->str == "x\xC3\xA4");                            // a-umlaut in UTF-8
    const std::string smiley = std::string("\"") + BS + "ud83d" + BS + "ude00\"";  // "😀"
    assert(Parse(smiley)->str == "\xF0\x9F\x98\x80");                     // U+1F600 in UTF-8

    assert(!Parse(""));
    assert(!Parse("{"));
    assert(!Parse(R"({"a": 1,})"));
    assert(!Parse(R"({"a": "unterminated)"));
    assert(!Parse("{} trailing"));
    assert(Parse("{}") && Parse("[]") && Parse("  42  "));
  }

  {
    namespace fs = feathercast::settings;

    fs::Settings original;
    original.shortcut = L"Ctrl+Shift+P";
    original.recentApps = {L"app:one", L"app:two"};
    original.pinnedApps = {L"app:pinned"};
    original.hiddenApps = {L"app:hidden"};
    original.appAliases[std::wstring(L"vs") + wchar_t(0xE4)] = L"line1\nline2\t\"quoted\"";  // key with a-umlaut
    original.usageStats[L"app:one"] = {42, 1750000000};
    original.compactMode = true;
    original.animationsEnabled = false;
    original.customAccentColor = L"#ff0000";
    original.lastUpdateAttempt = 1234567890000;
    original.lastUpdateCheck = 1234567890123;
    original.dismissedUpdateVersion = L"1.2.3";
    original.overlayWidth = 800;
    original.maxResults = 120;
    original.showStoreApps = false;
    original.privacyConsentVersion = 1;
    original.clipboardHistoryEnabled = true;
    original.clipboardHistoryLimit = 25;
    original.fileIndexEnabled = true;
    original.fileIndexMaxEntries = 2500;
    original.fileIndexRoots = {L"C:\\Work", L"D:\\Projects"};
    original.diagnosticsEnabled = true;
    original.quicklinks.push_back({L"docs", L"My \"Docs\"", L"C:\\Users\\Leon\\Docs"});

    const fs::Settings copy = fs::ParseSettings(fs::SerializeSettings(original));
    assert(copy.shortcut == original.shortcut);
    assert(copy.recentApps == original.recentApps);
    assert(copy.pinnedApps == original.pinnedApps);
    assert(copy.hiddenApps == original.hiddenApps);
    assert(copy.appAliases == original.appAliases);
    assert(copy.usageStats.size() == 1);
    assert(copy.usageStats.at(L"app:one").launches == 42);
    assert(copy.usageStats.at(L"app:one").lastUsed == 1750000000);
    assert(copy.compactMode == original.compactMode);
    assert(copy.animationsEnabled == original.animationsEnabled);
    assert(copy.customAccentColor == original.customAccentColor);
    assert(copy.lastUpdateAttempt == original.lastUpdateAttempt);
    assert(copy.lastUpdateCheck == original.lastUpdateCheck);
    assert(copy.dismissedUpdateVersion == original.dismissedUpdateVersion);
    assert(copy.overlayWidth == original.overlayWidth);
    assert(copy.maxResults == original.maxResults);
    assert(copy.showStoreApps == original.showStoreApps);
    assert(copy.privacyConsentVersion == original.privacyConsentVersion);
    assert(copy.clipboardHistoryEnabled == original.clipboardHistoryEnabled);
    assert(copy.clipboardHistoryLimit == original.clipboardHistoryLimit);
    assert(copy.fileIndexEnabled == original.fileIndexEnabled);
    assert(copy.fileIndexMaxEntries == original.fileIndexMaxEntries);
    assert(copy.fileIndexRoots == original.fileIndexRoots);
    assert(copy.diagnosticsEnabled == original.diagnosticsEnabled);
    assert(copy.searchEngines == original.searchEngines);
    assert(copy.quicklinks.size() == 1);
    assert(copy.quicklinks[0].keyword == L"docs");
    assert(copy.quicklinks[0].name == L"My \"Docs\"");
    assert(copy.quicklinks[0].target == L"C:\\Users\\Leon\\Docs");

    // Malformed/edge inputs fall back to defaults without crashing.
    assert(fs::ParseSettings("").shortcut == L"Alt+Space");
    assert(fs::ParseSettings("{}").maxResults == 200);
    assert(!fs::ParseSettings("{}").clipboardHistoryEnabled);
    assert(!fs::ParseSettings("{}").fileIndexEnabled);
    assert(fs::ParseSettings(R"({"shortcut": "Alt)").shortcut == L"Alt+Space");
    // A key name inside a string value must not be mistaken for the key
    // (the old substring scanner got this wrong).
    const auto tricky = fs::ParseSettings(R"({"shortcut": "\"recentApps\": [\"fake\"]"})");
    assert(tricky.recentApps.empty());
  }

  {
    namespace fd = feathercast::discovery;

    assert(fd::ShouldSkipName(L"Uninstall Foo"));
    assert(fd::ShouldSkipName(L"Hilfe zu X"));
    assert(!fd::ShouldSkipName(L"Notepad"));
    assert(fd::NameKey(L"Foo.lnk") == L"foo");
    assert(fd::CleanName(L"  Bar.lnk ") == L"Bar");
    assert(fd::BaseNameNoExt(L"C:\\Tools\\wt.exe") == L"wt");
    assert(fd::IsSystemEssentialName(L"Windows Terminal"));
    assert(!fd::IsSystemEssentialName(L"Notepad"));

    const auto unique = fd::UniqueKeywords({L"Visual Studio", L"visual x", L"a"});
    assert(unique == (std::vector<std::wstring>{L"visual", L"studio"}));  // dedup + drop 1-char words

    const auto terminalKeywords = fd::KeywordsFor(L"Windows Terminal", L"C:\\wt.exe", L"");
    const auto has = [&](const wchar_t* word) {
      return std::find(terminalKeywords.begin(), terminalKeywords.end(), word) != terminalKeywords.end();
    };
    assert(has(L"wt") && has(L"shell") && has(L"terminal"));
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Alt+Space");
  }

  {
    const auto altSpaceHotKey = ToHotKeySpec(ParseShortcut(L"Alt+Space"));
    assert(altSpaceHotKey.supported);
    assert(altSpaceHotKey.vk == VK_SPACE);
    assert((altSpaceHotKey.modifiers & MOD_ALT) != 0);
    assert((altSpaceHotKey.modifiers & MOD_CONTROL) == 0);
    assert((altSpaceHotKey.modifiers & 0x4000) != 0);

    const auto controlAltKHotKey = ToHotKeySpec(ParseShortcut(L"Control+Alt+K"));
    assert(controlAltKHotKey.supported);
    assert(controlAltKHotKey.vk == L'K');
    assert((controlAltKHotKey.modifiers & MOD_CONTROL) != 0);
    assert((controlAltKHotKey.modifiers & MOD_ALT) != 0);

    assert(!ToHotKeySpec(ParseShortcut(L"Super")).supported);
    assert(!ToHotKeySpec(ParseShortcut(L"none")).supported);

    assert(!ShouldHandleInLowLevelHook(ParseShortcut(L"Alt+Space"), true));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Alt+Space"), false));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Super"), false));
    assert(ShouldHandleInLowLevelHook(ParseShortcut(L"Super"), true));
    assert(!ShouldHandleInLowLevelHook(ParseShortcut(L"none"), false));
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_CONTROL, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Control+Space");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_CONTROL, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(L'K', true, false), L"Control+Alt+K");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_LWIN, true, false));
    AssertRecorded(recorder.Handle(VK_SPACE, true, false), L"Super+Space");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_LWIN, true, false));
    AssertRecorded(recorder.Handle(VK_LWIN, false, true), L"Super");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingCanceled(recorder.Handle(VK_ESCAPE, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecorded(recorder.Handle(VK_MENU, false, true), L"Alt");
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(L'K', true, false));
    AssertRecordingPending(recorder.Handle(VK_CAPITAL, true, false));
  }

  {
    ShortcutRecorder recorder;
    AssertRecordingPending(recorder.Handle(VK_MENU, true, false));
    AssertRecordingPending(recorder.Handle(VK_CAPITAL, true, false));
    AssertRecordingPending(recorder.Handle(VK_MENU, false, true));
  }

  {
    const auto super = ParseShortcut(L"Super");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(super, VK_LWIN, true, false, Mods(false, false, false, true)));
    const auto release = runtime.Handle(super, VK_LWIN, false, true, Mods());
    assert(!release.consume);
    assert(release.toggle);
    assert(release.suppressWinStart);
  }

  {
    const auto super = ParseShortcut(L"Super");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(super, VK_LWIN, true, false, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, L'E', true, false, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, L'E', false, true, Mods(false, false, false, true)));
    AssertPassOnly(runtime.Handle(super, VK_LWIN, false, true, Mods()));
  }

  {
    const auto superSpace = ParseShortcut(L"Super+Space");
    ShortcutRuntime runtime;
    AssertPassOnly(runtime.Handle(superSpace, VK_LWIN, true, false, Mods(false, false, false, true)));
    const auto open = runtime.Handle(superSpace, VK_SPACE, true, false, Mods(false, false, false, true));
    assert(open.consume);
    assert(open.toggle);
    assert(open.suppressWinStart);
    const auto repeat = runtime.Handle(superSpace, VK_SPACE, true, false, Mods(false, false, false, true));
    assert(repeat.consume);
    assert(!repeat.toggle);
    const auto release = runtime.Handle(superSpace, VK_SPACE, false, true, Mods(false, false, false, true));
    assert(release.consume);
    assert(!release.toggle);
    assert(!release.suppressWinStart);
  }

  {
    const auto altSpace = ParseShortcut(L"Alt+Space");
    ShortcutRuntime runtime;
    const auto open = runtime.Handle(altSpace, VK_SPACE, true, false, Mods(false, true));
    assert(open.consume);
    assert(open.toggle);
    const auto altSpaceRelease = runtime.Handle(altSpace, VK_SPACE, false, true, Mods(false, true));
    assert(altSpaceRelease.consume);
    assert(!altSpaceRelease.toggle);

    AssertPassOnly(runtime.Handle(altSpace, VK_RETURN, true, false, Mods(false, true)));

    const auto controlSpace = ParseShortcut(L"Control+Space");
    ShortcutRuntime controlRuntime;
    const auto controlOpen = controlRuntime.Handle(controlSpace, VK_SPACE, true, false, Mods(true));
    assert(controlOpen.consume);
    assert(controlOpen.toggle);
    const auto controlRelease = controlRuntime.Handle(controlSpace, VK_SPACE, false, true, Mods(true));
    assert(controlRelease.consume);
    assert(!controlRelease.toggle);

    const auto alt = ParseShortcut(L"Alt");
    ShortcutRuntime altRuntime;
    AssertPassOnly(altRuntime.Handle(alt, VK_MENU, true, false, Mods(false, true)));
    const auto altRelease = altRuntime.Handle(alt, VK_MENU, false, true, Mods());
    assert(altRelease.consume);
    assert(altRelease.toggle);
    assert(!altRelease.suppressWinStart);
  }

  return 0;
}
