#pragma once

#include "extension_protocol.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace feathercast::theme {

struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Theme {
  // Obsidian font stack: Segoe UI Variable (Win11 optical scaling) with fallbacks.
  std::wstring fontFamily = L"Segoe UI Variable Text, Segoe UI Variable, Inter, Segoe UI";
  // Obsidian dark panel at ~85 % opacity over real DirectComposition transparency.
  Color overlayBackground{0.063f, 0.063f, 0.071f, 0.85f};
  Color settingsBackground{0.063f, 0.063f, 0.071f, 0.85f};
  Color border{1.0f, 1.0f, 1.0f, 0.08f};
  Color divider{1.0f, 1.0f, 1.0f, 0.08f};
  // surface: #18181B – modern Tailwind Zinc-900 slate grey.
  Color surface{0.094f, 0.094f, 0.106f, 1.0f};
  Color surfaceHover{1.0f, 1.0f, 1.0f, 0.12f};
  // selectedBase: dark opaque base that the Windows accent is tinted into for a readable,
  // accent-colored selection pill.
  Color selectedBase{0.11f, 0.11f, 0.13f, 1.0f};
  Color iconTile{0.23f, 0.23f, 0.28f, 1.0f};
  Color textPrimary{0.95f, 0.95f, 0.96f, 1.0f};
  Color textMuted{0.60f, 0.60f, 0.64f, 1.0f};
  Color textDim{0.45f, 0.45f, 0.50f, 1.0f};
  Color sectionText{0.62f, 0.64f, 0.74f, 1.0f};
  Color danger{1.0f, 0.36f, 0.36f, 1.0f};
  Color accentFallback{0.36f, 0.42f, 1.0f, 1.0f};
  float overlayRadius = 10.0f;
  float settingsRadius = 10.0f;
  // rowRadius: 6.0f keeps selected rows subtly rounded inside the 10px outer panel.
  float rowRadius = 6.0f;
  float controlRadius = 8.0f;
};

inline int HexNibble(wchar_t ch) {
  if (ch >= L'0' && ch <= L'9') return ch - L'0';
  ch = static_cast<wchar_t>(std::towlower(ch));
  if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
  return -1;
}

inline std::optional<Color> ParseHexColor(const std::wstring& value, float fallbackAlpha = 1.0f) {
  if (value.size() != 7 && value.size() != 9) return std::nullopt;
  if (value.front() != L'#') return std::nullopt;
  auto byteAt = [&](size_t index) -> std::optional<int> {
    const int hi = HexNibble(value[index]);
    const int lo = HexNibble(value[index + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    return hi * 16 + lo;
  };
  const auto r = byteAt(1);
  const auto g = byteAt(3);
  const auto b = byteAt(5);
  if (!r || !g || !b) return std::nullopt;
  float alpha = std::clamp(fallbackAlpha, 0.0f, 1.0f);
  if (value.size() == 9) {
    const auto a = byteAt(7);
    if (!a) return std::nullopt;
    alpha = *a / 255.0f;
  }
  return Color{*r / 255.0f, *g / 255.0f, *b / 255.0f, alpha};
}

inline void ApplyColor(const std::string& json, const char* key, Color& target) {
  if (const auto raw = feathercast::extensions::JsonString(json, key)) {
    if (const auto parsed = ParseHexColor(feathercast::extensions::Utf8ToWide(*raw), target.a)) {
      target = *parsed;
    }
  }
}

inline void ApplyFloat(const std::string& json, const char* key, float& target, float minValue, float maxValue) {
  if (const auto raw = feathercast::extensions::JsonNumber(json, key)) {
    target = std::clamp(static_cast<float>(*raw), minValue, maxValue);
  }
}

inline Theme ParseThemeJson(const std::string& json, const Theme& defaults = Theme{}) {
  Theme theme = defaults;
  if (const auto font = feathercast::extensions::JsonString(json, "fontFamily")) {
    const std::wstring parsed = feathercast::extensions::Utf8ToWide(*font);
    if (!parsed.empty()) theme.fontFamily = parsed;
  }

  ApplyColor(json, "overlayBackground", theme.overlayBackground);
  ApplyColor(json, "settingsBackground", theme.settingsBackground);
  ApplyColor(json, "border", theme.border);
  ApplyColor(json, "divider", theme.divider);
  ApplyColor(json, "surface", theme.surface);
  ApplyColor(json, "surfaceHover", theme.surfaceHover);
  ApplyColor(json, "selectedBase", theme.selectedBase);
  ApplyColor(json, "iconTile", theme.iconTile);
  ApplyColor(json, "textPrimary", theme.textPrimary);
  ApplyColor(json, "textMuted", theme.textMuted);
  ApplyColor(json, "textDim", theme.textDim);
  ApplyColor(json, "sectionText", theme.sectionText);
  ApplyColor(json, "danger", theme.danger);
  ApplyColor(json, "accentFallback", theme.accentFallback);

  ApplyFloat(json, "overlayRadius", theme.overlayRadius, 0.0f, 32.0f);
  ApplyFloat(json, "settingsRadius", theme.settingsRadius, 0.0f, 32.0f);
  ApplyFloat(json, "rowRadius", theme.rowRadius, 0.0f, 20.0f);
  ApplyFloat(json, "controlRadius", theme.controlRadius, 0.0f, 20.0f);
  return theme;
}

inline Theme LoadTheme(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return Theme{};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return ParseThemeJson(buffer.str());
}

inline bool WriteDefaultTheme(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return true;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file <<
      "{\n"
      "  \"fontFamily\": \"Segoe UI Variable Text, Segoe UI Variable, Inter, Segoe UI\",\n"
      "  \"overlayBackground\": \"#101012D9\",\n"
      "  \"settingsBackground\": \"#101012D9\",\n"
      "  \"border\": \"#FFFFFF14\",\n"
      "  \"divider\": \"#FFFFFF14\",\n"
      "  \"surface\": \"#18181B\",\n"
      "  \"surfaceHover\": \"#FFFFFF1F\",\n"
      "  \"selectedBase\": \"#1C1C20\",\n"
      "  \"iconTile\": \"#3B3B47\",\n"
      "  \"textPrimary\": \"#F2F2F5\",\n"
      "  \"textMuted\": \"#9999A3\",\n"
      "  \"textDim\": \"#737380\",\n"
      "  \"sectionText\": \"#9EA3BD\",\n"
      "  \"danger\": \"#FF5C5C\",\n"
      "  \"accentFallback\": \"#5B6CFF\",\n"
      "  \"overlayRadius\": 10,\n"
      "  \"settingsRadius\": 10,\n"
      "  \"rowRadius\": 6,\n"
      "  \"controlRadius\": 8\n"
      "}\n";
  return true;
}

}  // namespace feathercast::theme
