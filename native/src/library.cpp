#include "library.hpp"

#include <algorithm>
#include <cwctype>
#include <numeric>

namespace feathercast::library {
namespace {

std::wstring Trim(std::wstring value) {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](wchar_t ch) {
                                        return std::iswspace(ch) != 0;
                                      });
  const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                     [](wchar_t ch) {
                                       return std::iswspace(ch) != 0;
                                     })
                        .base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

template <typename T, typename Key>
bool HasDuplicateKeyword(const T& values, const std::wstring& keyword,
                         std::optional<std::size_t> editingIndex, Key key) {
  const std::wstring normalized = NormalizeKeyword(keyword);
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (editingIndex && *editingIndex == index) continue;
    if (NormalizeKeyword(key(values[index])) == normalized) return true;
  }
  return false;
}

template <typename T, typename Name, typename Keyword>
std::vector<std::size_t> SortedIndices(const T& values, Name name,
                                       Keyword keyword) {
  std::vector<std::size_t> indices(values.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::stable_sort(indices.begin(), indices.end(), [&](std::size_t left,
                                                        std::size_t right) {
    std::wstring leftName = NormalizeKeyword(name(values[left]));
    std::wstring rightName = NormalizeKeyword(name(values[right]));
    if (leftName != rightName) return leftName < rightName;
    return NormalizeKeyword(keyword(values[left])) <
           NormalizeKeyword(keyword(values[right]));
  });
  return indices;
}

}  // namespace

std::wstring NormalizeKeyword(std::wstring value) {
  value = Trim(std::move(value));
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

std::optional<std::wstring> ValidateSnippet(
    const snippets::Snippet& candidate,
    const std::vector<snippets::Snippet>& existing,
    std::optional<std::size_t> editingIndex) {
  if (Trim(candidate.keyword).empty()) return L"Keyword is required.";
  if (Trim(candidate.name).empty()) return L"Name is required.";
  if (Trim(candidate.text).empty()) return L"Snippet text is required.";
  if (HasDuplicateKeyword(existing, candidate.keyword, editingIndex,
                          [](const auto& item) { return item.keyword; })) {
    return L"Another snippet already uses this keyword.";
  }
  return std::nullopt;
}

std::optional<std::wstring> ValidateQuicklink(
    const settings::Quicklink& candidate,
    const std::vector<settings::Quicklink>& existing,
    std::optional<std::size_t> editingIndex) {
  if (Trim(candidate.keyword).empty()) return L"Keyword is required.";
  if (Trim(candidate.target).empty()) return L"Target is required.";
  if (HasDuplicateKeyword(existing, candidate.keyword, editingIndex,
                          [](const auto& item) { return item.keyword; })) {
    return L"Another quicklink already uses this keyword.";
  }
  return std::nullopt;
}

std::optional<std::wstring> ValidateAppAlias(
    const AppAlias& candidate, const std::vector<AppAlias>& existing,
    std::optional<std::size_t> editingIndex) {
  if (Trim(candidate.appId).empty()) return L"App is required.";
  const std::wstring alias = Trim(candidate.alias);
  if (alias.empty()) return L"Alias is required.";
  if (alias.size() > 64) return L"Alias must not exceed 64 characters.";
  if (HasDuplicateKeyword(existing, alias, editingIndex,
                          [](const auto& item) { return item.alias; })) {
    return L"Another app already uses this alias.";
  }
  return std::nullopt;
}

std::optional<std::wstring> ValidateWebSearch(
    const WebSearch& candidate, const std::vector<WebSearch>& existing,
    std::optional<std::size_t> editingIndex) {
  const std::wstring keyword = NormalizeKeyword(candidate.keyword);
  if (keyword.empty()) return L"Keyword is required.";
  if (keyword.size() > 24) return L"Keyword must not exceed 24 characters.";
  if (std::any_of(keyword.begin(), keyword.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
      })) {
    return L"Keyword must not contain whitespace.";
  }
  const std::wstring url = Trim(candidate.urlTemplate);
  if (!url.starts_with(L"https://") && !url.starts_with(L"http://")) {
    return L"URL template must start with http:// or https://.";
  }
  if (url.size() > 2048) return L"URL template is too long.";
  const auto placeholder = url.find(L"%s");
  if (placeholder == std::wstring::npos ||
      url.find(L"%s", placeholder + 2) != std::wstring::npos) {
    return L"URL template must contain exactly one %s placeholder.";
  }
  if (HasDuplicateKeyword(existing, keyword, editingIndex,
                          [](const auto& item) { return item.keyword; })) {
    return L"Another web search already uses this keyword.";
  }
  return std::nullopt;
}

std::vector<std::size_t> SortedSnippetIndices(
    const std::vector<snippets::Snippet>& values) {
  return SortedIndices(
      values, [](const auto& item) { return item.name; },
      [](const auto& item) { return item.keyword; });
}

std::vector<std::size_t> SortedQuicklinkIndices(
    const std::vector<settings::Quicklink>& values) {
  return SortedIndices(
      values,
      [](const auto& item) {
        return item.name.empty() ? item.keyword : item.name;
      },
      [](const auto& item) { return item.keyword; });
}

std::vector<std::size_t> SortedAppAliasIndices(
    const std::vector<AppAlias>& values) {
  return SortedIndices(
      values, [](const auto& item) { return item.appName; },
      [](const auto& item) { return item.alias; });
}

std::vector<std::size_t> SortedWebSearchIndices(
    const std::vector<WebSearch>& values) {
  return SortedIndices(
      values, [](const auto& item) { return item.keyword; },
      [](const auto& item) { return item.urlTemplate; });
}

}  // namespace feathercast::library
