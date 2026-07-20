#include "search_pipeline.hpp"

#include "calculator.hpp"
#include "capability_catalog.hpp"
#include "converter.hpp"
#include "core.hpp"
#include "emoji.hpp"
#include "extension_protocol.hpp"
#include "run_command.hpp"
#include "search_scope.hpp"
#include "symbols.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace feathercast::search_pipeline {
namespace {

using namespace app;

std::wstring PrimaryAppId(const AppEntry& entry) {
  if (!entry.id.empty()) return entry.id;
  if (!entry.path.empty()) return entry.path;
  return entry.launchTarget;
}

DisplayItem CalculatorDisplay(const calculator::Result& calculation) {
  DisplayItem item;
  item.isCalculator = true;
  item.calculationExpression = calculation.expression;
  item.calculationResult = calculation.display;
  item.commandDetail = L"Calculator result - " + calculation.expression;
  item.commandKeywords = {L"calculator", L"calc", calculation.expression,
                          calculation.display};
  return item;
}

DisplayItem ConversionDisplay(const converter::Result& conversion) {
  DisplayItem item;
  item.isConversion = true;
  item.calculationExpression = conversion.expression;
  item.calculationResult = conversion.display;
  item.commandDetail = L"Conversion - " + conversion.expression;
  item.commandKeywords = {L"convert", L"conversion", conversion.expression,
                          conversion.display};
  return item;
}

std::string UrlEncode(const std::wstring& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::string utf8 = extensions::WideToUtf8(text);
  std::string out;
  for (const unsigned char ch : utf8) {
    if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else if (ch == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[ch >> 4]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

DisplayItem WebSearchDisplay(const std::wstring& keyword,
                             const std::wstring& engineTemplate,
                             const std::wstring& terms) {
  std::wstring url = engineTemplate;
  const std::wstring encoded = extensions::Utf8ToWide(UrlEncode(terms));
  if (const std::size_t position = url.find(L"%s");
      position != std::wstring::npos) {
    url.replace(position, 2, encoded);
  } else {
    url += encoded;
  }
  DisplayItem item;
  item.isWebSearch = true;
  item.webSearchUrl = url;
  item.webSearchLabel = L"Search " + keyword + L" for \"" + terms + L"\"";
  item.commandDetail = url;
  item.commandKeywords = {L"web", L"search", keyword, terms};
  return item;
}

DisplayItem RunCommandDisplay(const run_command::Command& command) {
  DisplayItem item;
  item.isRunCommand = true;
  item.runCommand = command;
  item.commandDetail = command.detail;
  item.commandKeywords = {L"run", L"command", L"shell", L"url",
                          command.input, command.target};
  return item;
}

DisplayItem SymbolDisplay(const symbols::Symbol& symbol) {
  DisplayItem item;
  item.isSymbol = true;
  item.symbol = symbol;
  item.commandDetail = L"Symbol - " + symbol.value;
  item.commandKeywords = symbol.keywords;
  item.commandKeywords.push_back(symbol.label);
  item.commandKeywords.push_back(symbol.value);
  return item;
}

DisplayItem UtilityDisplay(const clock_utilities::Result& utility,
                           UtilityKind kind) {
  DisplayItem item;
  item.utility = UtilityResult{kind, utility.stableId, utility.title,
                               utility.value, utility.keywords};
  item.commandDetail = L"Local utility - " + utility.value;
  item.commandKeywords = utility.keywords;
  item.commandKeywords.push_back(utility.value);
  return item;
}

UtilityKind UtilityKindFor(const std::wstring& stableId) {
  if (stableId == L"local-date") return UtilityKind::LocalDate;
  if (stableId == L"iso-week") return UtilityKind::IsoWeek;
  if (stableId == L"unix-time") return UtilityKind::UnixTime;
  return UtilityKind::LocalTime;
}

bool MatchesScope(const DisplayItem& item, search_scope::Scope scope) {
  const bool plainApp = !item.isWindow && !item.isCommand && !item.isSnippet &&
                        !item.isClipboard && !item.isAction &&
                        !item.isExtension && !item.utility;
  switch (scope) {
    case search_scope::Scope::All: return true;
    case search_scope::Scope::Apps:
      return plainApp && item.app.source != L"file" &&
             item.app.source != L"system-folder";
    case search_scope::Scope::Games:
      return plainApp && item.app.isGame;
    case search_scope::Scope::Windows: return item.isWindow;
    case search_scope::Scope::Files:
      return plainApp && item.app.source == L"file";
    case search_scope::Scope::Commands: return item.isCommand;
    case search_scope::Scope::Clipboard: return item.isClipboard;
    case search_scope::Scope::Snippets: return item.isSnippet;
  }
  return false;
}

std::wstring ScopeTitle(search_scope::Scope scope, bool empty) {
  using search_scope::Scope;
  switch (scope) {
    case Scope::Apps: return L"Apps";
    case Scope::Games: return L"Games";
    case Scope::Windows: return L"Open windows";
    case Scope::Files: return empty ? L"Recently modified" : L"Names & paths";
    case Scope::Commands: return L"Commands";
    case Scope::Clipboard: return L"Clipboard History";
    case Scope::Snippets: return L"Snippets";
    case Scope::All: return L"Results";
  }
  return L"Results";
}

DisplayItem ScopeSuggestion(const search_scope::Descriptor& descriptor) {
  DisplayItem item;
  item.isCapability = true;
  item.capability.stableId = L"scope:" + std::wstring(descriptor.token);
  item.capability.category = L"SEARCH SCOPES";
  item.capability.title = std::wstring(descriptor.token) + L" — " +
                          std::wstring(descriptor.label);
  item.capability.summary = std::wstring(descriptor.detail);
  item.capability.action.kind = CapabilityActionKind::SeedQuery;
  item.capability.action.query = std::wstring(descriptor.token) + L" ";
  return item;
}

}  // namespace

app::ResultsCollection ComputeResults(const app::QueryRequest& request) {
  using namespace app;
  ResultsCollection result;
  result.generation = request.generation;

  const SearchSnapshot emptySnapshot;
  const SearchSnapshot* snapshot =
      request.snapshot ? request.snapshot.get() : &emptySnapshot;
  std::vector<Section> sections;
  std::set<std::wstring> used;
  auto take = [&](const std::vector<DisplayItem>& items,
                  std::size_t limit = SIZE_MAX) {
    std::vector<DisplayItem> output;
    for (const auto& item : items) {
      const auto key = item.Key();
      if (key.empty() || used.contains(key)) continue;
      used.insert(key);
      output.push_back(item);
      if (output.size() >= limit) break;
    }
    return output;
  };
  auto addSection = [&](std::wstring title, std::vector<DisplayItem> items) {
    if (!items.empty()) {
      sections.push_back({std::move(title), std::move(items)});
    }
  };

  if (request.compactClear) {
    // Compact mode deliberately renders no results.
  } else if (request.browseView == BrowseView::Clipboard) {
    if (request.empty) {
      addSection(L"Clipboard History", take(snapshot->clipboardItems));
    } else {
      const auto order = core::Search(request.query,
                                      snapshot->clipboardSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(snapshot->clipboardItems[index]);
      addSection(L"Clipboard History", take(hits));
    }
  } else if (request.browseView == BrowseView::Emoji) {
    std::vector<DisplayItem> items;
    for (const auto& emoji : emoji::SearchEmoji(request.query, 300)) {
      items.push_back(SymbolDisplay(emoji));
    }
    addSection(L"Emoji", take(items));
  } else if (request.browseView == BrowseView::Games) {
    if (request.empty) {
      addSection(L"Games", take(snapshot->gameItems));
    } else {
      const auto order = core::Search(request.query, snapshot->gameSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(snapshot->gameItems[index]);
      addSection(L"Games", take(hits));
    }
  } else if (request.browseView == BrowseView::Capabilities) {
    std::map<std::wstring, std::vector<DisplayItem>> grouped;
    std::vector<std::wstring> categoryOrder;
    for (const auto* capability : capabilities::Search(request.query)) {
      if (!grouped.contains(capability->category)) {
        categoryOrder.push_back(capability->category);
      }
      grouped[capability->category].push_back(
          capabilities::Display(*capability));
    }
    for (const auto& category : categoryOrder) {
      addSection(category, take(grouped[category]));
    }
  } else if (request.actionMode) {
    if (request.empty) {
      addSection(L"Actions", take(request.actions));
    } else {
      const auto order = core::Search(request.query, request.actionSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(request.actions[index]);
      addSection(L"Actions", take(hits));
    }
  } else if (const auto suggestions = search_scope::Suggestions(request.query);
             !suggestions.empty()) {
    std::vector<DisplayItem> items;
    for (const auto* descriptor : suggestions) {
      items.push_back(ScopeSuggestion(*descriptor));
    }
    addSection(L"Search scopes", take(items));
  } else if (request.scope != search_scope::Scope::All) {
    std::vector<DisplayItem> hits;
    if (request.empty) {
      for (const auto& item : snapshot->pool) {
        if (MatchesScope(item, request.scope)) hits.push_back(item);
      }
      if (request.scope == search_scope::Scope::Files) {
        std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
          return left.app.fileLastWriteTime > right.app.fileLastWriteTime;
        });
      }
    } else {
      core::SearchOptions options;
      options.limit = snapshot->pool.size();
      options.now = request.now;
      options.generation = request.generation;
      options.latestGeneration = request.latestGeneration;
      const auto order = core::SearchPrepared(request.query,
                                               snapshot->searchItems,
                                               request.recentIds, options);
      for (const auto index : order) {
        if (MatchesScope(snapshot->pool[index], request.scope)) {
          hits.push_back(snapshot->pool[index]);
          if (hits.size() >= static_cast<std::size_t>(request.limit)) break;
        }
      }
    }
    addSection(ScopeTitle(request.scope, request.empty), take(hits));
  } else if (request.empty) {
    addSection(L"Pinned", take(snapshot->pinned, 12));
    addSection(L"Recently used", take(snapshot->recent, 8));
    addSection(L"Open windows", take(snapshot->windowItems));
    addSection(L"Snippets", take(snapshot->snippetItems, 8));
    addSection(L"Clipboard History", take(snapshot->clipboardItems, 5));
    addSection(L"System Folders", take(snapshot->systemFolders, 12));
    addSection(L"System essentials", take(snapshot->system, 8));
    addSection(L"Commands", take(snapshot->commandItems, 8));
    const auto discover = std::find_if(
        snapshot->commandItems.begin(), snapshot->commandItems.end(),
        [](const DisplayItem& item) {
          return item.isCommand &&
                 item.command == CommandKind::DiscoverFeatherCast;
        });
    if (discover != snapshot->commandItems.end()) {
      addSection(L"Explore", take({*discover}, 1));
    }
  } else {
    const std::wstring trimmed = core::Trim(request.query);
    if (trimmed.starts_with(L">")) {
      if (const auto command = run_command::Classify(trimmed)) {
        addSection(command->kind == run_command::Kind::OpenTarget ? L"Open"
                                                                  : L"Run",
                   take({RunCommandDisplay(*command)}, 1));
      }
    } else if (trimmed.starts_with(L":")) {
      std::vector<DisplayItem> items;
      for (const auto& symbol : symbols::SearchSymbols(trimmed, 40)) {
        items.push_back(SymbolDisplay(symbol));
      }
      addSection(L"Symbols", take(items, 40));
    } else {
      if (!trimmed.empty()) {
        const std::size_t space = trimmed.find_first_of(L" \t");
        if (space != std::wstring::npos) {
          const std::wstring keyword = core::Lower(trimmed.substr(0, space));
          const std::wstring terms = core::Trim(trimmed.substr(space + 1));
          if (!terms.empty()) {
            if (const auto engine = request.searchEngines.find(keyword);
                engine != request.searchEngines.end()) {
              addSection(L"Web Search",
                         take({WebSearchDisplay(keyword, engine->second, terms)},
                              1));
            }
          }
        }
      }

      if (const auto calculation = calculator::TryEvaluate(request.query)) {
        addSection(L"Calculator", take({CalculatorDisplay(*calculation)}, 1));
      }
      if (const auto conversion = converter::TryConvert(
              request.query, request.currencyRates, request.defaultCurrency)) {
        addSection(L"Conversion", take({ConversionDisplay(*conversion)}, 1));
      }
      if (const auto utility =
              clock_utilities::Evaluate(request.query, request.clock)) {
        addSection(L"Utilities",
                   take({UtilityDisplay(*utility,
                                        UtilityKindFor(utility->stableId))},
                        1));
      }
      addSection(L"Extensions", take(request.extensionItems, 20));

      core::SearchOptions options;
      options.limit = static_cast<std::size_t>(request.limit);
      options.now = request.now;
      options.generation = request.generation;
      options.latestGeneration = request.latestGeneration;
      const auto order = core::SearchPrepared(request.query,
                                               snapshot->searchItems,
                                               request.recentIds, options);
      std::vector<DisplayItem> hits;
      hits.reserve(order.size());
      for (const auto index : order) hits.push_back(snapshot->pool[index]);
      if (!hits.empty()) addSection(L"Best match", take({hits.front()}, 1));

      std::vector<DisplayItem> rest(
          hits.size() > 1 ? hits.begin() + 1 : hits.end(), hits.end());
      std::vector<DisplayItem> recent;
      std::vector<DisplayItem> games;
      std::vector<DisplayItem> apps;
      std::vector<DisplayItem> windows;
      std::vector<DisplayItem> system;
      std::vector<DisplayItem> commands;
      std::vector<DisplayItem> quicklinks;
      std::vector<DisplayItem> snippets;
      std::vector<DisplayItem> clipboard;
      std::vector<DisplayItem> files;
      std::vector<DisplayItem> systemFolders;
      std::vector<DisplayItem> other;
      for (const auto& item : rest) {
        const bool plainApp = !item.isWindow && !item.isCommand &&
                              !item.isSnippet && !item.isClipboard &&
                              !item.utility;
        if (item.isSnippet) snippets.push_back(item);
        if (item.isClipboard) clipboard.push_back(item);
        if (item.isCommand) commands.push_back(item);
        if (plainApp && item.app.source == L"quicklink") quicklinks.push_back(item);
        if (plainApp && item.app.source == L"file") files.push_back(item);
        if (plainApp && item.app.source == L"system-folder") {
          systemFolders.push_back(item);
        }
        if (plainApp && item.app.isGame) games.push_back(item);
        if (plainApp && item.app.source != L"file" &&
            request.recentIds.contains(PrimaryAppId(item.app))) {
          recent.push_back(item);
        }
        if (plainApp && !item.app.isGame && item.app.source == L"shortcut") {
          apps.push_back(item);
        }
        if (item.isWindow) windows.push_back(item);
        if (plainApp && !item.app.isGame && item.app.source != L"shortcut" &&
            item.app.source != L"quicklink" && item.app.source != L"file" &&
            item.app.source != L"system-folder") {
          system.push_back(item);
        }
        other.push_back(item);
      }
      addSection(L"Commands", take(commands, 20));
      addSection(L"Quicklinks", take(quicklinks, 20));
      addSection(L"Snippets", take(snippets, 20));
      addSection(L"Clipboard History", take(clipboard, 20));
      addSection(L"Recently used", take(recent, 8));
      addSection(L"Games", take(games, 80));
      addSection(L"Apps", take(apps, 80));
      addSection(L"Files & Folders", take(files, 40));
      addSection(L"System Folders", take(systemFolders, 30));
      addSection(L"Open windows", take(windows, 40));
      addSection(L"System & Store apps", take(system, 80));
      addSection(L"Other matches", take(other, 40));
    }
  }

  for (const auto& section : sections) {
    result.flatItems.insert(result.flatItems.end(), section.items.begin(),
                            section.items.end());
  }
  result.sections = std::move(sections);
  return result;
}

}  // namespace feathercast::search_pipeline
