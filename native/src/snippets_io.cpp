#include "snippets_io.hpp"

#include "extension_protocol.hpp"
#include "json.hpp"
#include "settings.hpp"

#include <windows.h>

#include <array>
#include <fstream>
#include <sstream>

namespace feathercast::snippets_io {
namespace {

bool ReadString(const json::Value& object, const char* key,
                std::wstring& output) {
  const auto* value = object.Find(key);
  if (!value || value->type != json::Value::Type::String) return false;
  output = extensions::Utf8ToWide(value->str);
  return true;
}

}  // namespace

FileFingerprint Inspect(const std::filesystem::path& path) {
  FileFingerprint fingerprint;
  std::error_code ec;
  fingerprint.exists = std::filesystem::exists(path, ec);
  if (ec) return {};
  fingerprint.inspected = true;
  if (!fingerprint.exists) return fingerprint;
  fingerprint.size = std::filesystem::file_size(path, ec);
  if (ec) return {};
  fingerprint.writeTime = std::filesystem::last_write_time(path, ec);
  if (ec) return {};
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::uint64_t hash = 14695981039346656037ULL;
  std::array<char, 8192> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    for (std::streamsize index = 0; index < file.gcount(); ++index) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
      hash *= 1099511628211ULL;
    }
  }
  if (!file.eof()) return {};
  fingerprint.contentHash = hash;
  return fingerprint;
}

LoadResult Load(const std::filesystem::path& path) {
  LoadResult result;
  result.fingerprint = Inspect(path);
  if (!result.fingerprint.inspected) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not inspect snippets.json. Editing is disabled.";
    return result;
  }
  if (!result.fingerprint.exists) return result;

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not read snippets.json. Editing is disabled.";
    return result;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.good() && !file.eof()) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not finish reading snippets.json. Editing is disabled.";
    return result;
  }

  const auto root = json::Parse(buffer.str());
  const auto* items = root && root->type == json::Value::Type::Object
                          ? root->Find("snippets")
                          : nullptr;
  if (!items || items->type != json::Value::Type::Array) {
    result.status = LoadStatus::Invalid;
    result.message = L"snippets.json is invalid. Open or reload the file before editing.";
    return result;
  }

  for (const auto& value : items->array) {
    snippets::Snippet snippet;
    if (value.type != json::Value::Type::Object ||
        !ReadString(value, "keyword", snippet.keyword) ||
        !ReadString(value, "name", snippet.name) ||
        !ReadString(value, "text", snippet.text) ||
        snippets::Trim(snippet.keyword).empty() ||
        snippets::Trim(snippet.name).empty() ||
        snippets::Trim(snippet.text).empty()) {
      result.snippets.clear();
      result.status = LoadStatus::Invalid;
      result.message = L"snippets.json contains an invalid entry. Editing is disabled.";
      return result;
    }
    snippet.keyword = snippets::Trim(std::move(snippet.keyword));
    snippet.name = snippets::Trim(std::move(snippet.name));
    result.snippets.push_back(std::move(snippet));
  }
  result.status = LoadStatus::Valid;
  return result;
}

std::string Serialize(const std::vector<snippets::Snippet>& snippets) {
  std::ostringstream out;
  out << "{\n  \"snippets\": [";
  for (std::size_t index = 0; index < snippets.size(); ++index) {
    if (index) out << ",";
    const auto& snippet = snippets[index];
    out << "\n    {\"keyword\": \"" << settings::JsonEscape(snippet.keyword)
        << "\", \"name\": \"" << settings::JsonEscape(snippet.name)
        << "\", \"text\": \"" << settings::JsonEscape(snippet.text)
        << "\"}";
  }
  if (!snippets.empty()) out << "\n  ";
  out << "]\n}\n";
  return out.str();
}

SaveResult Save(const std::filesystem::path& path,
                const std::vector<snippets::Snippet>& snippets,
                const FileFingerprint& expected) {
  const auto current = Inspect(path);
  if (!current.inspected) {
    return {false, current,
            L"Could not inspect snippets.json. Reload it before saving."};
  }
  if (!(current == expected)) {
    return {false, current,
            L"snippets.json changed outside FeatherCast. Reload it before saving."};
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return {false, expected, L"Could not create the snippets directory."};
  }
  auto temporary = path;
  temporary += L".tmp";
  const std::string serialized = Serialize(snippets);
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return {false, expected, L"Could not create snippets.json.tmp."};
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    file.flush();
    if (!file) {
      file.close();
      std::filesystem::remove(temporary, ec);
      return {false, expected, L"Could not finish writing snippets.json."};
    }
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary, ec);
    return {false, expected, L"Could not replace snippets.json."};
  }
  return {true, Inspect(path), L"Library saved."};
}

}  // namespace feathercast::snippets_io
