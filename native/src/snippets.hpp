#pragma once

#include "extension_protocol.hpp"

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::snippets {

struct Snippet {
  std::wstring keyword;
  std::wstring name;
  std::wstring text;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::vector<Snippet> ParseSnippetsJson(const std::string& json) {
  std::vector<Snippet> out;
  for (const auto& object : feathercast::extensions::JsonObjectArray(json, "snippets")) {
    auto keyword = feathercast::extensions::JsonString(object, "keyword");
    auto name = feathercast::extensions::JsonString(object, "name");
    auto text = feathercast::extensions::JsonString(object, "text");
    if (!keyword || !name || !text) continue;

    Snippet snippet;
    snippet.keyword = Trim(feathercast::extensions::Utf8ToWide(*keyword));
    snippet.name = Trim(feathercast::extensions::Utf8ToWide(*name));
    snippet.text = feathercast::extensions::Utf8ToWide(*text);
    if (snippet.keyword.empty() || snippet.name.empty() || Trim(snippet.text).empty()) continue;
    out.push_back(std::move(snippet));
  }
  return out;
}

}  // namespace feathercast::snippets
