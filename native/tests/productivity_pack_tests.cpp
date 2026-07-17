#include "clock_utilities.hpp"
#include "audio_volume.hpp"
#include "command_catalog.hpp"
#include "core.hpp"
#include "search_pipeline.hpp"
#include "test_framework.hpp"
#include "system_settings.hpp"
#include "window_layout.hpp"
#include "uuid_utilities.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using feathercast::window_layout::Layout;
using feathercast::window_layout::Rect;

bool HasAction(const std::vector<feathercast::app::DisplayItem>& actions,
               feathercast::app::ActionKind kind) {
  return std::any_of(actions.begin(), actions.end(), [&](const auto& item) {
    return item.isAction && item.action == kind;
  });
}

void AssertTextActions(const feathercast::app::DisplayItem& item,
                       const std::wstring& expectedValue) {
  const auto actions = feathercast::commands::BuildActions(
      item, feathercast::app::Settings{});
  assert(actions.size() == 2);
  assert(actions[0].action == feathercast::app::ActionKind::CopyText);
  assert(actions[1].action == feathercast::app::ActionKind::PasteText);
  for (const auto& action : actions) {
    const auto* payload =
        std::get_if<feathercast::app::TextActionPayload>(&action.actionTarget);
    assert(payload && payload->value == expectedValue);
  }
}

}  // namespace

int main() {
  assert(feathercast::audio::ClampPercent(-1) == 0);
  assert(feathercast::audio::ClampPercent(42) == 42);
  assert(feathercast::audio::ClampPercent(101) == 100);
  assert(feathercast::audio::AdjustPercent(0, -1) == 0);
  assert(feathercast::audio::AdjustPercent(50, -10) == 40);
  assert(feathercast::audio::AdjustPercent(50, 10) == 60);
  assert(feathercast::audio::AdjustPercent(100, 1) == 100);
  assert(feathercast::audio::PercentFromTrack(-10.0f, 0.0f, 200.0f) == 0);
  assert(feathercast::audio::PercentFromTrack(100.0f, 0.0f, 200.0f) == 50);
  assert(feathercast::audio::PercentFromTrack(210.0f, 0.0f, 200.0f) == 100);
  assert(feathercast::audio::PercentFromTrack(10.0f, 10.0f, 10.0f) == 0);

  const Rect work{-1920, 0, 1, 1081};
  const Rect window{-1700, 100, -700, 800};
  assert(feathercast::window_layout::Compute(Layout::LeftHalf, window, work,
                                              work) ==
         (Rect{-1920, 0, -960, 1081}));
  assert(feathercast::window_layout::Compute(Layout::RightHalf, window, work,
                                              work) ==
         (Rect{-960, 0, 1, 1081}));
  assert(feathercast::window_layout::Compute(Layout::TopHalf, window, work,
                                              work) ==
         (Rect{-1920, 0, 1, 540}));
  assert(feathercast::window_layout::Compute(Layout::BottomHalf, window, work,
                                              work) ==
         (Rect{-1920, 540, 1, 1081}));
  assert(feathercast::window_layout::Compute(
             Layout::Center, Rect{0, 0, 3000, 2000}, work, work) == work);

  const Rect nextWork{0, -200, 1280, 824};
  const Rect moved = feathercast::window_layout::Compute(
      Layout::NextDisplay, window, work, nextWork);
  assert(moved.Width() == window.Width());
  assert(moved.Height() == window.Height());
  assert(moved.left >= nextWork.left && moved.right <= nextWork.right);
  assert(moved.top >= nextWork.top && moved.bottom <= nextWork.bottom);
  assert(feathercast::window_layout::PositionLess(
      Rect{-1920, 0, 0, 1080}, Rect{0, -200, 1280, 824}));
  assert(feathercast::window_layout::NextIndex(0, 3) == 1);
  assert(feathercast::window_layout::NextIndex(2, 3) == 0);
  assert(feathercast::window_layout::NextIndex(0, 0) == 0);
  assert(feathercast::window_layout::Compute(
             Layout::NextDisplay, Rect{200, 200, 400, 400},
             Rect{0, 0, 1000, 1000}, Rect{1000, 100, 3000, 1100}) ==
         (Rect{1500, 300, 1700, 500}));

  feathercast::clock_utilities::ClockSnapshot clock;
  clock.localDate = std::chrono::year{2026} / std::chrono::July / 17;
  clock.localHour = 9;
  clock.localMinute = 5;
  clock.localSecond = 7;
  clock.epochSeconds = 1784288707;
  assert(feathercast::clock_utilities::Evaluate(L" time ", clock)->value ==
         L"09:05:07");
  assert(feathercast::clock_utilities::Evaluate(L"today", clock)->value ==
         L"2026-07-17");
  assert(feathercast::clock_utilities::Evaluate(L"week number", clock)->value ==
         L"2026-W29");
  assert(feathercast::clock_utilities::Evaluate(L"unix timestamp", clock)->value ==
         L"1784288707");
  assert(!feathercast::clock_utilities::Evaluate(L"not a utility", clock));

  GUID knownGuid{0x00112233, 0x4455, 0x4677,
                 {0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
  assert(feathercast::uuid_utilities::Format(knownGuid) ==
         L"00112233-4455-4677-8899-aabbccddeeff");

  clock.localDate = std::chrono::year{2021} / std::chrono::January / 1;
  assert(feathercast::clock_utilities::Evaluate(L"iso week", clock)->value ==
         L"2020-W53");
  clock.localDate = std::chrono::year{2024} / std::chrono::February / 29;
  assert(feathercast::clock_utilities::Evaluate(L"date", clock)->value ==
         L"2024-02-29");

  const std::vector<std::tuple<feathercast::app::CommandKind, std::wstring,
                               std::wstring>> expectedCommands = {
      {feathercast::app::CommandKind::VolumeControl, L"volume-control",
       L"volume"},
      {feathercast::app::CommandKind::VolumeUp, L"volume-up", L"audio"},
      {feathercast::app::CommandKind::VolumeDown, L"volume-down", L"audio"},
      {feathercast::app::CommandKind::MediaPlayPause, L"media-play-pause",
       L"media"},
      {feathercast::app::CommandKind::MediaNext, L"media-next", L"next"},
      {feathercast::app::CommandKind::MediaPrevious, L"media-previous",
       L"previous"},
      {feathercast::app::CommandKind::ShowDesktop, L"show-desktop",
       L"desktop"},
      {feathercast::app::CommandKind::GenerateUuid, L"generate-uuid", L"uuid"},
  };
  for (const auto& [kind, stableId, keyword] : expectedCommands) {
    const auto* descriptor = feathercast::commands::Find(kind);
    assert(descriptor && descriptor->stableId == stableId);
    assert(!descriptor->confirmation);
    assert(std::find(descriptor->keywords.begin(), descriptor->keywords.end(),
                     keyword) != descriptor->keywords.end());
  }

  const auto settingsTargets = feathercast::system_settings::Catalog();
  assert(settingsTargets.size() == 5);
  std::set<std::wstring> settingsIds;
  std::vector<feathercast::core::SearchItem> settingsSearchItems;
  for (const auto& target : settingsTargets) {
    assert(target.id.starts_with(L"windows-settings:"));
    assert(settingsIds.insert(target.id).second);
    assert(target.source == L"windows-settings");
    assert(target.launchType == feathercast::app::LaunchType::Shell);
    assert(target.launchTarget.starts_with(L"ms-settings:"));
    assert(target.systemEssential);
    feathercast::core::SearchItem searchItem;
    searchItem.id = target.id;
    searchItem.name = target.name;
    searchItem.keywords = target.keywords;
    settingsSearchItems.push_back(std::move(searchItem));
  }
  for (const auto& [query, expectedId] :
       std::vector<std::pair<std::wstring, std::wstring>>{
           {L"display", L"windows-settings:display"},
           {L"sound", L"windows-settings:sound"},
           {L"bluetooth", L"windows-settings:bluetooth"},
           {L"installed apps", L"windows-settings:installed-apps"},
           {L"windows update", L"windows-settings:windows-update"},
       }) {
    const auto matches = feathercast::core::Search(query, settingsSearchItems);
    assert(!matches.empty());
    assert(settingsSearchItems[matches.front()].id == expectedId);
  }

  feathercast::app::DisplayItem windowItem;
  windowItem.isWindow = true;
  windowItem.window.hwnd = reinterpret_cast<HWND>(0x1234);
  windowItem.window.name = L"Test Window";
  const auto windowActions = feathercast::commands::BuildActions(
      windowItem, feathercast::app::Settings{});
  for (const auto kind : {
           feathercast::app::ActionKind::MoveWindowLeftHalf,
           feathercast::app::ActionKind::MoveWindowRightHalf,
           feathercast::app::ActionKind::MoveWindowTopHalf,
           feathercast::app::ActionKind::MoveWindowBottomHalf,
           feathercast::app::ActionKind::CenterWindow,
           feathercast::app::ActionKind::MoveWindowNextDisplay,
  }) {
    assert(HasAction(windowActions, kind));
  }
  for (const auto& action : windowActions) {
    const auto* target =
        std::get_if<feathercast::app::WindowEntry>(&action.actionTarget);
    assert(target && target->hwnd == windowItem.window.hwnd);
  }

  feathercast::app::DisplayItem calculation;
  calculation.isCalculator = true;
  calculation.calculationExpression = L"6 * 7";
  calculation.calculationResult = L"42";
  const auto calculationActions = feathercast::commands::BuildActions(
      calculation, feathercast::app::Settings{});
  assert(calculationActions.size() == 3);
  assert(HasAction(calculationActions,
                   feathercast::app::ActionKind::CopyText));
  assert(HasAction(calculationActions,
                   feathercast::app::ActionKind::PasteText));
  const auto* complete = std::get_if<feathercast::app::TextActionPayload>(
      &calculationActions[1].actionTarget);
  assert(complete && complete->value == L"6 * 7 = 42");

  feathercast::app::DisplayItem conversion = calculation;
  conversion.isCalculator = false;
  conversion.isConversion = true;
  conversion.calculationExpression = L"1 km to m";
  conversion.calculationResult = L"1,000 m";
  const auto conversionActions = feathercast::commands::BuildActions(
      conversion, feathercast::app::Settings{});
  assert(conversionActions.size() == 3);
  const auto* conversionComplete =
      std::get_if<feathercast::app::TextActionPayload>(
          &conversionActions[1].actionTarget);
  assert(conversionComplete &&
         conversionComplete->value == L"1 km to m = 1,000 m");

  feathercast::app::DisplayItem snippet;
  snippet.isSnippet = true;
  snippet.snippet.text = L"Reusable text";
  AssertTextActions(snippet, snippet.snippet.text);
  feathercast::app::DisplayItem clipboard;
  clipboard.isClipboard = true;
  clipboard.clipboard.text = L"Clipboard text";
  AssertTextActions(clipboard, clipboard.clipboard.text);
  feathercast::app::DisplayItem symbol;
  symbol.isSymbol = true;
  symbol.symbol.value = L"\u221e";
  AssertTextActions(symbol, symbol.symbol.value);
  feathercast::app::DisplayItem utility;
  utility.utility = feathercast::app::UtilityResult{
      feathercast::app::UtilityKind::LocalTime, L"local-time", L"Local Time",
      L"12:34:56", {L"time"}};
  AssertTextActions(utility, utility.utility->value);

  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  feathercast::app::DisplayItem notepad;
  notepad.app.id = L"app:notepad";
  notepad.app.name = L"Notepad";
  notepad.app.source = L"shortcut";
  snapshot->pool.push_back(notepad);
  feathercast::core::SearchItem searchItem;
  searchItem.id = notepad.app.id;
  searchItem.name = notepad.app.name;
  searchItem.source = notepad.app.source;
  searchItem.kind = L"app";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(searchItem));

  feathercast::app::DisplayItem volumeCommand;
  volumeCommand.isCommand = true;
  volumeCommand.command = feathercast::app::CommandKind::VolumeUp;
  volumeCommand.commandName = L"Volume Up";
  snapshot->pool.push_back(volumeCommand);
  feathercast::core::SearchItem commandSearchItem;
  commandSearchItem.id = L"cmd:volume-up";
  commandSearchItem.name = volumeCommand.commandName;
  commandSearchItem.keywords = {L"volume", L"audio", L"louder"};
  commandSearchItem.source = L"command";
  commandSearchItem.kind = L"command";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(commandSearchItem));

  feathercast::app::DisplayItem volumeControlCommand;
  volumeControlCommand.isCommand = true;
  volumeControlCommand.command = feathercast::app::CommandKind::VolumeControl;
  volumeControlCommand.commandName = L"Volume Control";
  snapshot->pool.push_back(volumeControlCommand);
  feathercast::core::SearchItem volumeControlSearchItem;
  volumeControlSearchItem.id = L"cmd:volume-control";
  volumeControlSearchItem.name = volumeControlCommand.commandName;
  volumeControlSearchItem.keywords = {L"volume", L"audio", L"speaker"};
  volumeControlSearchItem.source = L"command";
  volumeControlSearchItem.kind = L"command";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(volumeControlSearchItem));

  snapshot->pool.push_back(utility);
  feathercast::core::SearchItem utilitySearchItem;
  utilitySearchItem.id = utility.Key();
  utilitySearchItem.name = utility.Name();
  utilitySearchItem.keywords = utility.utility->keywords;
  utilitySearchItem.source = L"utility";
  utilitySearchItem.kind = L"utility";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(utilitySearchItem));
  snapshot->pool.push_back(notepad);
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(searchItem));

  feathercast::app::QueryRequest request;
  request.generation = 44;
  request.limit = 200;
  request.snapshot = snapshot;
  request.query = L"time";
  request.clock = clock;
  request.clock.localDate =
      std::chrono::year{2026} / std::chrono::December / 31;
  request.clock.localHour = 23;
  request.clock.localMinute = 59;
  request.clock.localSecond = 58;
  const auto utilityResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(utilityResults.generation == request.generation);
  assert(!utilityResults.sections.empty());
  assert(utilityResults.sections.front().title == L"Utilities");
  assert(utilityResults.flatItems.front().utility);
  assert(utilityResults.flatItems.front().utility->value == L"23:59:58");
  assert(std::count_if(utilityResults.flatItems.begin(),
                       utilityResults.flatItems.end(), [](const auto& item) {
                         return item.Key() == L"utility:local-time";
                       }) == 1);
  const auto repeated = feathercast::search_pipeline::ComputeResults(request);
  assert(repeated.flatItems.front().utility->value == L"23:59:58");

  for (const auto& [query, expected] :
       std::vector<std::pair<std::wstring, std::wstring>>{
           {L"date", L"2026-12-31"},
           {L"iso week", L"2026-W53"},
           {L"unix time", L"1784288707"},
       }) {
    request.query = query;
    const auto results = feathercast::search_pipeline::ComputeResults(request);
    assert(!results.flatItems.empty() && results.flatItems.front().utility);
    assert(results.flatItems.front().utility->value == expected);
  }

  request.query = L"notepad";
  const auto appResults = feathercast::search_pipeline::ComputeResults(request);
  assert(std::count_if(appResults.flatItems.begin(), appResults.flatItems.end(),
                       [](const auto& item) {
                         return item.app.id == L"app:notepad";
                       }) == 1);

  request.query = L"volume up";
  const auto commandResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!commandResults.flatItems.empty());
  assert(commandResults.flatItems.front().isCommand);
  assert(commandResults.flatItems.front().command ==
         feathercast::app::CommandKind::VolumeUp);

  request.query = L"volume control";
  const auto volumeControlResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!volumeControlResults.flatItems.empty());
  assert(volumeControlResults.flatItems.front().isCommand);
  assert(volumeControlResults.flatItems.front().command ==
         feathercast::app::CommandKind::VolumeControl);

  std::atomic<unsigned long long> newest{request.generation + 1};
  request.query = L"notepad";
  request.latestGeneration = &newest;
  const auto staleResults = feathercast::search_pipeline::ComputeResults(request);
  assert(std::none_of(staleResults.flatItems.begin(), staleResults.flatItems.end(),
                      [](const auto& item) {
                        return item.app.id == L"app:notepad";
                      }));

  return 0;
}
