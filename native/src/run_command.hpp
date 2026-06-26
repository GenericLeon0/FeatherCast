#pragma once

#include "core.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::run_command {

enum class Kind {
  None,
  OpenTarget,
  ShellCommand,
};

struct Command {
  Kind kind = Kind::None;
  std::wstring input;
  std::wstring target;
  std::wstring label;
  std::wstring detail;
};

inline std::wstring StripMatchingQuotes(std::wstring value) {
  value = feathercast::core::Trim(std::move(value));
  if (value.size() >= 2 && ((value.front() == L'"' && value.back() == L'"') ||
                            (value.front() == L'\'' && value.back() == L'\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return feathercast::core::Trim(std::move(value));
}

inline bool ContainsWhitespace(const std::wstring& value) {
  return std::any_of(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
}

inline bool HasKnownUriScheme(const std::wstring& value) {
  const size_t colon = value.find(L':');
  if (colon == std::wstring::npos || colon == 0) return false;
  const std::wstring scheme = feathercast::core::Lower(value.substr(0, colon));
  static const std::vector<std::wstring> kSchemes = {
    L"http", L"https", L"file", L"ftp", L"mailto", L"ms-settings", L"shell",
    L"steam", L"vscode", L"obsidian", L"onenote", L"zoommtg",
  };
  return std::find(kSchemes.begin(), kSchemes.end(), scheme) != kSchemes.end();
}

inline bool LooksLikeHost(const std::wstring& host) {
  if (host.empty()) return false;
  if (host == L"localhost") return true;
  const bool ipv4Like = std::all_of(host.begin(), host.end(), [](wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || ch == L'.';
  });
  if (ipv4Like && host.find(L'.') != std::wstring::npos) return true;
  if (host.front() == L'.' || host.back() == L'.' || host.find(L'.') == std::wstring::npos) return false;
  return std::all_of(host.begin(), host.end(), [](wchar_t ch) {
    return std::iswalnum(ch) != 0 || ch == L'.' || ch == L'-';
  });
}

inline bool LooksLikeBareUrl(const std::wstring& value) {
  if (value.empty() || ContainsWhitespace(value) || value.find(L'\\') != std::wstring::npos) return false;
  const size_t slash = value.find(L'/');
  const size_t question = value.find(L'?');
  const size_t hash = value.find(L'#');
  size_t hostEnd = value.size();
  for (const size_t marker : {slash, question, hash}) {
    if (marker != std::wstring::npos) hostEnd = std::min(hostEnd, marker);
  }
  std::wstring host = value.substr(0, hostEnd);
  const size_t at = host.rfind(L'@');
  if (at != std::wstring::npos) host = host.substr(at + 1);
  const size_t colon = host.find(L':');
  if (colon != std::wstring::npos) host = host.substr(0, colon);
  return LooksLikeHost(feathercast::core::Lower(host));
}

inline std::optional<Command> Classify(std::wstring query) {
  query = feathercast::core::Trim(std::move(query));
  if (query.empty() || query.front() != L'>') return std::nullopt;

  std::wstring input = StripMatchingQuotes(query.substr(1));
  if (input.empty()) return std::nullopt;

  std::wstring openTarget;
  if (HasKnownUriScheme(input)) {
    openTarget = input;
  } else {
    const std::wstring unquoted = StripMatchingQuotes(input);
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(unquoted), ec)) {
      openTarget = unquoted;
    } else if (LooksLikeBareUrl(input)) {
      openTarget = L"https://" + input;
    }
  }

  if (!openTarget.empty()) {
    return Command{
      Kind::OpenTarget,
      input,
      openTarget,
      L"Open " + openTarget,
      openTarget,
    };
  }

  return Command{
    Kind::ShellCommand,
    input,
    input,
    L"Run command",
    input,
  };
}

}  // namespace feathercast::run_command
