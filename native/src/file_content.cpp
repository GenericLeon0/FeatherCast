#include "file_content.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <set>

namespace feathercast::file_content {
namespace {

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return extension;
}

bool ValidUtf8(const std::vector<unsigned char>& bytes, std::size_t offset) {
  for (std::size_t i = offset; i < bytes.size();) {
    const unsigned char first = bytes[i];
    if (first <= 0x7f) {
      ++i;
    } else if (first >= 0xc2 && first <= 0xdf) {
      if (i + 1 >= bytes.size() || (bytes[i + 1] & 0xc0) != 0x80) return false;
      i += 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      if (i + 2 >= bytes.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80) return false;
      if ((first == 0xe0 && bytes[i + 1] < 0xa0) ||
          (first == 0xed && bytes[i + 1] >= 0xa0)) return false;
      i += 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      if (i + 3 >= bytes.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80 || (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      if ((first == 0xf0 && bytes[i + 1] < 0x90) ||
          (first == 0xf4 && bytes[i + 1] >= 0x90)) return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

std::optional<std::wstring> DecodeCodePage(const unsigned char* data,
                                           std::size_t size, UINT codePage,
                                           DWORD flags) {
  if (size == 0) return std::wstring{};
  const int needed = MultiByteToWideChar(codePage, flags,
                                         reinterpret_cast<const char*>(data),
                                         static_cast<int>(size), nullptr, 0);
  if (needed <= 0) return std::nullopt;
  std::wstring text(static_cast<std::size_t>(needed), L'\0');
  if (MultiByteToWideChar(codePage, flags,
                          reinterpret_cast<const char*>(data),
                          static_cast<int>(size), text.data(), needed) <= 0) {
    return std::nullopt;
  }
  return text;
}

bool LooksBinary(const std::vector<unsigned char>& bytes) {
  if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) return true;
  std::size_t controls = 0;
  for (const auto byte : bytes) {
    if (byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t' &&
        byte != '\f' && byte != '\b') {
      ++controls;
    }
  }
  return !bytes.empty() && controls * 20 > bytes.size();
}

bool LooksBinaryText(const std::wstring& text) {
  if (text.find(L'\0') != std::wstring::npos) return true;
  std::size_t controls = 0;
  for (const wchar_t ch : text) {
    if (ch < 0x20 && ch != L'\r' && ch != L'\n' && ch != L'\t' &&
        ch != L'\f' && ch != L'\b') {
      ++controls;
    }
  }
  return !text.empty() && controls * 20 > text.size();
}

}  // namespace

bool Supports(const std::filesystem::path& path) {
  static const std::set<std::wstring> extensions = {
      L".txt", L".md", L".log", L".csv", L".tsv", L".json", L".xml",
      L".yaml", L".yml", L".ini", L".cfg", L".toml", L".c", L".cc",
      L".cpp", L".cxx", L".h", L".hh", L".hpp", L".hxx", L".cs",
      L".java", L".kt", L".py", L".js", L".jsx", L".ts", L".tsx",
      L".html", L".htm", L".css", L".scss", L".sql", L".ps1",
      L".bat", L".cmd", L".sh"};
  return extensions.contains(LowerExtension(path));
}

bool IsImage(const std::filesystem::path& path) {
  static const std::set<std::wstring> extensions = {
      L".bmp", L".gif", L".ico", L".jpeg", L".jpg", L".png", L".tif",
      L".tiff"};
  return extensions.contains(LowerExtension(path));
}

Extraction Extract(const std::filesystem::path& path, std::size_t maxBytes,
                   std::stop_token token) {
  Extraction result;
  if (!Supports(path)) {
    result.state = State::Unsupported;
    return result;
  }

  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    result.state = State::Unavailable;
    return result;
  }
  bool cloudOnly = (attributes & FILE_ATTRIBUTE_OFFLINE) != 0;
#ifdef FILE_ATTRIBUTE_RECALL_ON_OPEN
  cloudOnly = cloudOnly || (attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0;
#endif
#ifdef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
  cloudOnly =
      cloudOnly || (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
#endif
  if (cloudOnly) {
    result.state = State::CloudOnly;
    return result;
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    result.state = State::Unavailable;
    return result;
  }
  result.sourceBytes = static_cast<std::size_t>(size);
  if (size > maxBytes) {
    result.state = State::TooLarge;
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.state = State::Unavailable;
    return result;
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (token.stop_requested()) {
      result.state = State::Unavailable;
      return result;
    }
    const auto chunk = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset),
               static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      result.state = State::Unavailable;
      return result;
    }
    offset += chunk;
  }

  if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
    const std::size_t chars = (bytes.size() - 2) / 2;
    result.text.resize(chars);
    for (std::size_t i = 0; i < chars; ++i) {
      result.text[i] = static_cast<wchar_t>(
          bytes[2 + i * 2] | (static_cast<unsigned>(bytes[3 + i * 2]) << 8));
    }
  } else if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
    const std::size_t chars = (bytes.size() - 2) / 2;
    result.text.resize(chars);
    for (std::size_t i = 0; i < chars; ++i) {
      result.text[i] = static_cast<wchar_t>(
          (static_cast<unsigned>(bytes[2 + i * 2]) << 8) | bytes[3 + i * 2]);
    }
  } else {
    const std::size_t bomOffset = bytes.size() >= 3 && bytes[0] == 0xef &&
                                          bytes[1] == 0xbb && bytes[2] == 0xbf
                                      ? 3
                                      : 0;
    if (LooksBinary(bytes)) {
      result.state = State::Binary;
      return result;
    }
    auto decoded = ValidUtf8(bytes, bomOffset)
                       ? DecodeCodePage(bytes.data() + bomOffset,
                                        bytes.size() - bomOffset, CP_UTF8,
                                        MB_ERR_INVALID_CHARS)
                       : DecodeCodePage(bytes.data(), bytes.size(), CP_ACP, 0);
    if (!decoded) {
      result.state = State::Binary;
      return result;
    }
    result.text = std::move(*decoded);
  }

  if (LooksBinaryText(result.text)) {
    result.text.clear();
    result.state = State::Binary;
    return result;
  }
  result.state = State::Indexed;
  return result;
}

}  // namespace feathercast::file_content
