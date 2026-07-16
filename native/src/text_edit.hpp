#pragma once

#include <algorithm>
#include <cwctype>
#include <string>

namespace feathercast::text_edit {

inline bool IsHighSurrogate(wchar_t ch) {
  return ch >= 0xD800 && ch <= 0xDBFF;
}

inline bool IsLowSurrogate(wchar_t ch) {
  return ch >= 0xDC00 && ch <= 0xDFFF;
}

inline size_t PreviousCodePoint(const std::wstring& text, size_t position) {
  position = std::min(position, text.size());
  if (position == 0) return 0;
  size_t previous = position - 1;
  if (previous > 0 && IsLowSurrogate(text[previous]) && IsHighSurrogate(text[previous - 1])) --previous;
  return previous;
}

inline size_t NextCodePoint(const std::wstring& text, size_t position) {
  position = std::min(position, text.size());
  if (position >= text.size()) return text.size();
  size_t next = position + 1;
  if (next < text.size() && IsHighSurrogate(text[position]) && IsLowSurrogate(text[next])) ++next;
  return next;
}

inline size_t PreviousWord(const std::wstring& text, size_t position) {
  position = PreviousCodePoint(text, position);
  while (position > 0 && std::iswspace(text[position])) position = PreviousCodePoint(text, position);
  while (position > 0) {
    const size_t previous = PreviousCodePoint(text, position);
    if (std::iswspace(text[previous])) break;
    position = previous;
  }
  return position;
}

inline size_t NextWord(const std::wstring& text, size_t position) {
  position = std::min(position, text.size());
  while (position < text.size() && std::iswspace(text[position])) position = NextCodePoint(text, position);
  while (position < text.size() && !std::iswspace(text[position])) position = NextCodePoint(text, position);
  return position;
}

inline bool ErasePrevious(std::wstring& text, size_t& position) {
  position = std::min(position, text.size());
  const size_t previous = PreviousCodePoint(text, position);
  if (previous == position) return false;
  text.erase(previous, position - previous);
  position = previous;
  return true;
}

inline bool EraseNext(std::wstring& text, size_t position) {
  position = std::min(position, text.size());
  const size_t next = NextCodePoint(text, position);
  if (next == position) return false;
  text.erase(position, next - position);
  return true;
}

}  // namespace feathercast::text_edit
