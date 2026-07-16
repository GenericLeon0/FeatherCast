#pragma once

// Minimal recursive-descent JSON parser for small config files (settings,
// currency cache). Parses the full document instead of substring-scanning, so
// key names inside string values can never be mistaken for keys.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::json {

struct Member;

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<Value> array;
  std::vector<Member> object;

  const Value* Find(std::string_view key) const;
};

struct Member {
  std::string key;
  Value value;
};

inline const Value* Value::Find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& member : object) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

namespace detail {

struct Parser {
  std::string_view text;
  size_t pos = 0;
  int depth = 0;
  static constexpr int kMaxDepth = 64;

  void SkipWs() {
    while (pos < text.size()) {
      const char ch = text[pos];
      if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
      ++pos;
    }
  }

  bool Literal(std::string_view word) {
    if (text.compare(pos, word.size(), word) != 0) return false;
    pos += word.size();
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseHex4(uint32_t& out) {
    if (pos + 4 > text.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = text[pos++];
      out <<= 4;
      if (ch >= '0' && ch <= '9') out |= static_cast<uint32_t>(ch - '0');
      else if (ch >= 'a' && ch <= 'f') out |= static_cast<uint32_t>(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') out |= static_cast<uint32_t>(ch - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool ParseString(std::string& out) {
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    while (pos < text.size()) {
      const char ch = text[pos];
      if (ch == '"') {
        ++pos;
        return true;
      }
      if (ch == '\\') {
        ++pos;
        if (pos >= text.size()) return false;
        const char esc = text[pos++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: expect a low one
              if (pos + 1 >= text.size() || text[pos] != '\\' || text[pos + 1] != 'u') return false;
              pos += 2;
              uint32_t low = 0;
              if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return false;  // lone low surrogate
            }
            AppendUtf8(out, cp);
            break;
          }
          default:
            return false;
        }
        continue;
      }
      if (static_cast<unsigned char>(ch) < 0x20) return false;  // raw control char
      out.push_back(ch);
      ++pos;
    }
    return false;  // unterminated
  }

  bool ParseNumber(Value& out) {
    size_t end = pos;
    if (end < text.size() && text[end] == '-') ++end;
    while (end < text.size()) {
      const char ch = text[end];
      if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') ++end;
      else break;
    }
    double parsed = 0;
    const auto result = std::from_chars(text.data() + pos, text.data() + end, parsed);
    if (result.ec != std::errc{} || result.ptr == text.data() + pos) return false;
    pos = static_cast<size_t>(result.ptr - text.data());
    out.type = Value::Type::Number;
    out.number = parsed;
    return true;
  }

  bool ParseValue(Value& out) {
    if (++depth > kMaxDepth) return false;
    SkipWs();
    if (pos >= text.size()) return false;
    bool ok = false;
    const char ch = text[pos];
    if (ch == '"') {
      out.type = Value::Type::String;
      ok = ParseString(out.str);
    } else if (ch == '{') {
      ++pos;
      out.type = Value::Type::Object;
      ok = ParseMembers(out.object);
    } else if (ch == '[') {
      ++pos;
      out.type = Value::Type::Array;
      ok = ParseElements(out.array);
    } else if (ch == 't') {
      out.type = Value::Type::Bool;
      out.boolean = true;
      ok = Literal("true");
    } else if (ch == 'f') {
      out.type = Value::Type::Bool;
      out.boolean = false;
      ok = Literal("false");
    } else if (ch == 'n') {
      out.type = Value::Type::Null;
      ok = Literal("null");
    } else {
      ok = ParseNumber(out);
    }
    --depth;
    return ok;
  }

  bool ParseMembers(std::vector<Member>& out) {
    SkipWs();
    if (pos < text.size() && text[pos] == '}') {
      ++pos;
      return true;
    }
    while (pos < text.size()) {
      SkipWs();
      Member member;
      if (!ParseString(member.key)) return false;
      SkipWs();
      if (pos >= text.size() || text[pos] != ':') return false;
      ++pos;
      if (!ParseValue(member.value)) return false;
      out.push_back(std::move(member));
      SkipWs();
      if (pos >= text.size()) return false;
      if (text[pos] == ',') {
        ++pos;
        continue;
      }
      if (text[pos] == '}') {
        ++pos;
        return true;
      }
      return false;
    }
    return false;
  }

  bool ParseElements(std::vector<Value>& out) {
    SkipWs();
    if (pos < text.size() && text[pos] == ']') {
      ++pos;
      return true;
    }
    while (pos < text.size()) {
      Value element;
      if (!ParseValue(element)) return false;
      out.push_back(std::move(element));
      SkipWs();
      if (pos >= text.size()) return false;
      if (text[pos] == ',') {
        ++pos;
        continue;
      }
      if (text[pos] == ']') {
        ++pos;
        return true;
      }
      return false;
    }
    return false;
  }
};

}  // namespace detail

inline std::optional<Value> Parse(std::string_view text) {
  detail::Parser parser{text};
  Value value;
  if (!parser.ParseValue(value)) return std::nullopt;
  parser.SkipWs();
  if (parser.pos != text.size()) return std::nullopt;  // trailing garbage
  return value;
}

}  // namespace feathercast::json
