#pragma once

#include "core.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::search_scope {

enum class Scope {
  All,
  Apps,
  Windows,
  Files,
  Commands,
  Clipboard,
  Snippets,
};

struct Descriptor {
  Scope scope = Scope::All;
  std::wstring_view token;
  std::wstring_view label;
  std::wstring_view detail;
};

inline constexpr std::array<Descriptor, 6> kDescriptors = {{
    {Scope::Files, L"@files", L"Files", L"Search file names, paths, and enabled content"},
    {Scope::Apps, L"@apps", L"Apps", L"Search installed apps and quicklinks"},
    {Scope::Windows, L"@windows", L"Windows", L"Search open windows"},
    {Scope::Commands, L"@commands", L"Commands", L"Search FeatherCast commands"},
    {Scope::Clipboard, L"@clipboard", L"Clipboard", L"Search clipboard history"},
    {Scope::Snippets, L"@snippets", L"Snippets", L"Search reusable snippets"},
}};

struct ParsedQuery {
  Scope scope = Scope::All;
  std::wstring terms;
  std::wstring token;
  bool recognized = false;
  bool suggestionMode = false;
};

inline ParsedQuery Parse(std::wstring_view query) {
  ParsedQuery parsed;
  const std::wstring trimmed = feathercast::core::Trim(std::wstring(query));
  if (trimmed.empty() || trimmed.front() != L'@') {
    parsed.terms = trimmed;
    return parsed;
  }

  const auto separator = trimmed.find_first_of(L" \t");
  const std::wstring token = feathercast::core::Lower(
      trimmed.substr(0, separator == std::wstring::npos ? trimmed.size()
                                                        : separator));
  for (const auto& descriptor : kDescriptors) {
    if (token != descriptor.token) continue;
    parsed.scope = descriptor.scope;
    parsed.token = token;
    parsed.recognized = true;
    parsed.terms = separator == std::wstring::npos
                       ? L""
                       : feathercast::core::Trim(trimmed.substr(separator + 1));
    return parsed;
  }

  parsed.terms = trimmed;
  parsed.token = token;
  parsed.suggestionMode = separator == std::wstring::npos;
  return parsed;
}

inline std::vector<const Descriptor*> Suggestions(std::wstring_view query) {
  const auto parsed = Parse(query);
  if (!parsed.suggestionMode) return {};
  std::vector<const Descriptor*> suggestions;
  for (const auto& descriptor : kDescriptors) {
    if (parsed.token.empty() || std::wstring(descriptor.token).starts_with(parsed.token)) {
      suggestions.push_back(&descriptor);
    }
  }
  return suggestions;
}

inline std::wstring Token(Scope scope) {
  for (const auto& descriptor : kDescriptors) {
    if (descriptor.scope == scope) return std::wstring(descriptor.token);
  }
  return L"";
}

}  // namespace feathercast::search_scope
