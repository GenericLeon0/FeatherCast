#include "capability_catalog.hpp"

#include "core.hpp"

#include <set>
#include <utility>

namespace feathercast::capabilities {
namespace {

app::CapabilityAction Seed(std::wstring query) {
  app::CapabilityAction action;
  action.kind = app::CapabilityActionKind::SeedQuery;
  action.query = std::move(query);
  return action;
}

app::CapabilityAction Browse(app::BrowseView view) {
  app::CapabilityAction action;
  action.kind = app::CapabilityActionKind::OpenBrowse;
  action.browseView = view;
  return action;
}

app::CapabilityAction Settings(app::SettingsCategory category) {
  app::CapabilityAction action;
  action.kind = app::CapabilityActionKind::OpenSettings;
  action.settingsCategory = category;
  return action;
}

app::CapabilityAction Command(app::CommandKind command) {
  app::CapabilityAction action;
  action.kind = app::CapabilityActionKind::RunCommand;
  action.command = command;
  return action;
}

}  // namespace

const std::vector<CapabilityDescriptor>& Catalog() {
  using app::BrowseView;
  using app::CommandKind;
  using app::SettingsCategory;
  static const std::vector<CapabilityDescriptor> capabilities = {
      {L"apps", L"GET STARTED", L"Launch apps",
       L"Find installed desktop, Start Menu, Store, and system apps.",
       {L"applications", L"start menu", L"programs", L"launch"},
       L"notepad", Seed(L"notepad")},
      {L"windows", L"GET STARTED", L"Switch windows",
       L"Find an open window and bring it to the foreground.",
       {L"open windows", L"switch", L"focus", L"minimize", L"close"},
       L"terminal", Seed(L"terminal")},
      {L"actions", L"GET STARTED", L"Result actions",
       L"Press Tab or Ctrl+K for pin, hide, administrator, and window actions.",
       {L"keyboard", L"tab", L"ctrl k", L"pin", L"administrator"},
       L"settings", Seed(L"settings")},
      {L"calculator", L"SEARCH & TOOLS", L"Calculate",
       L"Type an arithmetic expression and copy the result with Enter.",
       {L"math", L"arithmetic", L"expression", L"copy result"},
       L"128 * 4", Seed(L"128 * 4")},
      {L"conversions", L"SEARCH & TOOLS", L"Convert units and currency",
       L"Convert measurements or cached currency rates directly in search.",
       {L"units", L"currency", L"measurement", L"exchange"},
       L"10 km to mi", Seed(L"10 km to mi")},
      {L"web-search", L"SEARCH & TOOLS", L"Search the web",
       L"Use a configured prefix such as g, ddg, yt, gh, or w.",
       {L"browser", L"google", L"duckduckgo", L"youtube", L"prefix"},
       L"g FeatherCast", Seed(L"g FeatherCast")},
      {L"emoji", L"SEARCH & TOOLS", L"Search emoji",
       L"Browse emoji and paste the selected character into the previous app.",
       {L"picker", L"emoticon", L"smiley", L"paste"}, L"smile",
       Browse(BrowseView::Emoji)},
      {L"symbols", L"SEARCH & TOOLS", L"Search symbols",
       L"Find common technical and typographic symbols and paste them.",
       {L"characters", L"unicode", L"typography", L"paste"},
       L"arrow", Seed(L"arrow")},
      {L"files", L"PERSONAL CONTENT", L"Find files and folders",
       L"Enable the local index and choose folders from Privacy settings.",
       {L"documents", L"folders", L"index", L"local", L"privacy"}, L"",
       Settings(SettingsCategory::Privacy)},
      {L"clipboard", L"PERSONAL CONTENT", L"Clipboard history",
       L"Search and paste locally protected clipboard text after opting in.",
       {L"copy", L"paste", L"history", L"privacy", L"dpapi"}, L"",
       Browse(BrowseView::Clipboard)},
      {L"snippets", L"PERSONAL CONTENT", L"Use snippets",
       L"Search reusable text snippets stored in snippets.json.",
       {L"text expansion", L"template", L"snippets json", L"paste"}, L"",
       Command(CommandKind::OpenSnippetsFile)},
      {L"quicklinks", L"PERSONAL CONTENT", L"Use quicklinks",
       L"Open configured URLs, files, and folders by keyword.",
       {L"bookmark", L"keyword", L"url", L"folder", L"settings json"},
       L"", Command(CommandKind::OpenDataFolder)},
      {L"system-commands", L"SYSTEM & EXTENSIONS", L"Run system commands",
       L"Lock, sleep, mute, restart, shut down, and perform maintenance safely.",
       {L"computer", L"power", L"lock", L"sleep", L"mute", L"maintenance"},
       L"lock pc", Seed(L"lock pc")},
      {L"plugins", L"SYSTEM & EXTENSIONS", L"Use native extensions",
       L"Install trusted native plugins that add searchable results and actions.",
       {L"extensions", L"dll", L"plugin host", L"custom results"}, L"",
       Settings(SettingsCategory::Extensions)},
      {L"shortcut", L"SYSTEM & EXTENSIONS", L"Change the global shortcut",
       L"Record the key combination that opens FeatherCast from anywhere.",
       {L"hotkey", L"keyboard", L"alt space", L"open launcher"}, L"",
       Settings(SettingsCategory::Shortcut)},
  };
  return capabilities;
}

std::vector<const CapabilityDescriptor*> Search(const std::wstring& query) {
  const auto& catalog = Catalog();
  std::vector<core::SearchItem> items;
  items.reserve(catalog.size());
  for (const auto& descriptor : catalog) {
    core::SearchItem item;
    item.id = descriptor.stableId;
    item.name = descriptor.title;
    item.source = descriptor.category;
    item.keywords = descriptor.keywords;
    item.keywords.push_back(descriptor.summary);
    item.keywords.push_back(descriptor.example);
    items.push_back(std::move(item));
  }
  const auto matches = core::Search(query, items);
  std::vector<const CapabilityDescriptor*> results;
  results.reserve(matches.size());
  for (const auto index : matches) results.push_back(&catalog[index]);
  return results;
}

app::DisplayItem Display(const CapabilityDescriptor& descriptor) {
  app::DisplayItem item;
  item.isCapability = true;
  item.capability.stableId = descriptor.stableId;
  item.capability.category = descriptor.category;
  item.capability.title = descriptor.title;
  item.capability.summary = descriptor.summary;
  item.capability.example = descriptor.example;
  item.capability.action = descriptor.action;
  return item;
}

bool ValidateCatalog(std::wstring* error) {
  std::set<std::wstring> ids;
  for (const auto& capability : Catalog()) {
    if (capability.stableId.empty() || capability.category.empty() ||
        capability.title.empty() || capability.summary.empty() ||
        !ids.insert(capability.stableId).second) {
      if (error) {
        *error = L"Capability identifiers must be unique and visible metadata must be complete.";
      }
      return false;
    }
    if (capability.action.kind == app::CapabilityActionKind::OpenBrowse &&
        capability.action.browseView == app::BrowseView::None) {
      if (error) *error = L"Browse capabilities require a browse destination.";
      return false;
    }
    if (capability.action.kind == app::CapabilityActionKind::SeedQuery &&
        capability.action.query.empty()) {
      if (error) *error = L"Query capabilities require a non-empty example.";
      return false;
    }
  }
  return true;
}

}  // namespace feathercast::capabilities
