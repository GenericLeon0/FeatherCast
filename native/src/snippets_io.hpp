#pragma once

#include "snippets.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::snippets_io {

enum class LoadStatus { Missing, Valid, Invalid };

struct FileFingerprint {
  bool inspected = false;
  bool exists = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type writeTime{};
  std::uint64_t contentHash = 0;

  bool operator==(const FileFingerprint&) const = default;
};

struct LoadResult {
  std::vector<snippets::Snippet> snippets;
  LoadStatus status = LoadStatus::Missing;
  FileFingerprint fingerprint;
  std::wstring message;

  bool Writable() const noexcept { return status != LoadStatus::Invalid; }
};

struct SaveResult {
  bool succeeded = false;
  FileFingerprint fingerprint;
  std::wstring message;
};

FileFingerprint Inspect(const std::filesystem::path& path);
LoadResult Load(const std::filesystem::path& path);
std::string Serialize(const std::vector<snippets::Snippet>& snippets);
SaveResult Save(const std::filesystem::path& path,
                const std::vector<snippets::Snippet>& snippets,
                const FileFingerprint& expected);

}  // namespace feathercast::snippets_io
