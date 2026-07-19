#pragma once

#include "settings.hpp"
#include "snippets.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::library {

enum class ItemKind { Snippet, Quicklink, AppAlias, WebSearch };

struct AppAlias {
  std::wstring appId;
  std::wstring appName;
  std::wstring alias;
};

struct AppChoice {
  std::wstring id;
  std::wstring name;
};

struct WebSearch {
  std::wstring keyword;
  std::wstring urlTemplate;
};

struct OperationResult {
  bool succeeded = false;
  std::wstring message;
};

std::wstring NormalizeKeyword(std::wstring value);

std::optional<std::wstring> ValidateSnippet(
    const snippets::Snippet& candidate,
    const std::vector<snippets::Snippet>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateQuicklink(
    const settings::Quicklink& candidate,
    const std::vector<settings::Quicklink>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateAppAlias(
    const AppAlias& candidate, const std::vector<AppAlias>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateWebSearch(
    const WebSearch& candidate, const std::vector<WebSearch>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::vector<std::size_t> SortedSnippetIndices(
    const std::vector<snippets::Snippet>& snippets);
std::vector<std::size_t> SortedQuicklinkIndices(
    const std::vector<settings::Quicklink>& quicklinks);
std::vector<std::size_t> SortedAppAliasIndices(
    const std::vector<AppAlias>& aliases);
std::vector<std::size_t> SortedWebSearchIndices(
    const std::vector<WebSearch>& searches);

}  // namespace feathercast::library
