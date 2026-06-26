#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace feathercast::calculator {

struct Result {
  std::wstring expression;
  std::wstring display;
  double value = 0.0;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring FormatNumber(double value) {
  if (!std::isfinite(value)) return L"";
  if (std::fabs(value) < 0.0000000001) value = 0.0;

  std::wostringstream stream;
  const double magnitude = std::fabs(value);
  if (magnitude >= 10000000000.0 || (magnitude > 0.0 && magnitude < 0.000001)) {
    stream << std::setprecision(10) << value;
    return stream.str();
  }

  stream << std::fixed << std::setprecision(10) << value;
  std::wstring out = stream.str();
  while (!out.empty() && out.back() == L'0') out.pop_back();
  if (!out.empty() && out.back() == L'.') out.pop_back();
  if (out == L"-0") out = L"0";
  return out;
}

class Parser {
 public:
  explicit Parser(std::wstring text) : text_(std::move(text)) {}

  bool Parse(double& value) {
    SkipSpaces();
    Parsed parsed;
    if (!ParseExpression(parsed)) return false;
    value = parsed.value;
    SkipSpaces();
    return pos_ == text_.size() && sawValue_ && sawCalculationSyntax_ && std::isfinite(value);
  }

 private:
  struct Parsed {
    double value = 0.0;
    bool percent = false;
  };

  static constexpr double kPi = 3.14159265358979323846;

  bool ParseExpression(Parsed& value) {
    if (!ParseTerm(value)) return false;
    while (true) {
      SkipSpaces();
      if (Match(L'+')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseTerm(rhs)) return false;
        const double base = value.value;
        value.value += rhs.percent ? base * rhs.value : rhs.value;
        value.percent = false;
      } else if (Match(L'-')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseTerm(rhs)) return false;
        const double base = value.value;
        value.value -= rhs.percent ? base * rhs.value : rhs.value;
        value.percent = false;
      } else {
        return true;
      }
    }
  }

  bool ParseTerm(Parsed& value) {
    if (!ParsePower(value)) return false;
    while (true) {
      SkipSpaces();
      if ((!Peek(L"**") && Match(L'*')) || Match(L'x') || Match(L'X')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParsePower(rhs)) return false;
        value.value *= rhs.value;
        value.percent = false;
      } else if (Match(L'/') || Match(L':')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParsePower(rhs) || std::fabs(rhs.value) < 0.0000000001) return false;
        value.value /= rhs.value;
        value.percent = false;
      } else {
        return true;
      }
    }
  }

  bool ParsePower(Parsed& value) {
    if (!ParseFactor(value)) return false;
    SkipSpaces();
    if (Match(L"**") || Match(L'^')) {
      sawCalculationSyntax_ = true;
      Parsed rhs;
      if (!ParsePower(rhs)) return false;
      value.value = std::pow(value.value, rhs.value);
      value.percent = false;
      return std::isfinite(value.value);
    }
    return true;
  }

  bool ParseFactor(Parsed& value) {
    SkipSpaces();
    if (Match(L'+')) return ParseFactor(value);
    if (Match(L'-')) {
      if (!ParseFactor(value)) return false;
      value.value = -value.value;
      return true;
    }

    if (Match(L'(')) {
      sawCalculationSyntax_ = true;
      if (!ParseExpression(value)) return false;
      SkipSpaces();
      if (!Match(L')')) return false;
    } else if (!ParseFunctionOrConstant(value) && !ParseNumber(value)) {
      return false;
    }

    while (true) {
      SkipSpaces();
      if (!Match(L'%')) return true;
      sawCalculationSyntax_ = true;
      value.value /= 100.0;
      value.percent = true;
    }
  }

  bool ParseFunctionOrConstant(Parsed& value) {
    SkipSpaces();
    const size_t start = pos_;
    while (pos_ < text_.size() && std::iswalpha(text_[pos_])) ++pos_;
    if (start == pos_) return false;

    std::wstring name = text_.substr(start, pos_ - start);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
      return static_cast<wchar_t>(std::towlower(ch));
    });

    if (name == L"pi") {
      value = Parsed{kPi, false};
      sawValue_ = true;
      return true;
    }
    if (name == L"e") {
      value = Parsed{std::exp(1.0), false};
      sawValue_ = true;
      return true;
    }

    SkipSpaces();
    if (!Match(L'(')) return false;
    sawCalculationSyntax_ = true;
    Parsed argument;
    if (!ParseExpression(argument)) return false;
    SkipSpaces();
    if (!Match(L')')) return false;

    if (name == L"sin" || name == L"cos" || name == L"tan") {
      const double radians = argument.value * kPi / 180.0;
      if (name == L"sin") value.value = std::sin(radians);
      else if (name == L"cos") value.value = std::cos(radians);
      else value.value = std::tan(radians);
      value.percent = false;
      return std::isfinite(value.value);
    }
    if (name == L"sqrt") {
      if (argument.value < 0.0) return false;
      value = Parsed{std::sqrt(argument.value), false};
      return true;
    }

    return false;
  }

  bool ParseNumber(Parsed& value) {
    SkipSpaces();
    const size_t start = pos_;
    bool hasDigit = false;
    bool hasSeparator = false;
    while (pos_ < text_.size()) {
      const wchar_t ch = text_[pos_];
      if (std::iswdigit(ch)) {
        hasDigit = true;
        ++pos_;
      } else if ((ch == L'.' || ch == L',') && !hasSeparator) {
        hasSeparator = true;
        ++pos_;
      } else {
        break;
      }
    }

    if (!hasDigit) return false;
    sawValue_ = true;
    std::wstring number = text_.substr(start, pos_ - start);
    std::replace(number.begin(), number.end(), L',', L'.');
    wchar_t* end = nullptr;
    value.value = std::wcstod(number.c_str(), &end);
    value.percent = false;
    return end && *end == L'\0' && std::isfinite(value.value);
  }

  void SkipSpaces() {
    while (pos_ < text_.size() && std::iswspace(text_[pos_])) ++pos_;
  }

  bool Match(wchar_t expected) {
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool Match(const wchar_t* expected) {
    if (!Peek(expected)) return false;
    pos_ += std::wcslen(expected);
    return true;
  }

  bool Peek(const wchar_t* expected) const {
    const size_t len = std::wcslen(expected);
    return pos_ + len <= text_.size() && text_.compare(pos_, len, expected) == 0;
  }

  std::wstring text_;
  size_t pos_ = 0;
  bool sawValue_ = false;
  bool sawCalculationSyntax_ = false;
};

inline std::optional<Result> TryEvaluate(std::wstring input) {
  input = Trim(std::move(input));
  if (input.empty()) return std::nullopt;
  if (input.front() == L'=') input = Trim(input.substr(1));
  if (input.empty()) return std::nullopt;

  double value = 0.0;
  Parser parser(input);
  if (!parser.Parse(value)) return std::nullopt;

  std::wstring display = FormatNumber(value);
  if (display.empty()) return std::nullopt;
  return Result{input, display, value};
}

}  // namespace feathercast::calculator
