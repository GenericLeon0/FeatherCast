#pragma once

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace feathercast::file_content {

inline constexpr std::size_t kMaxIndexedBytes = 2 * 1024 * 1024;
inline constexpr long long kTotalSourceQuotaBytes = 256LL * 1024 * 1024;

enum class State {
  NotRequested = 0,
  Indexed = 1,
  Unsupported = 2,
  TooLarge = 3,
  Binary = 4,
  Unavailable = 5,
  CloudOnly = 6,
};

struct Extraction {
  State state = State::Unavailable;
  std::wstring text;
  std::size_t sourceBytes = 0;
};

bool Supports(const std::filesystem::path& path);
bool IsImage(const std::filesystem::path& path);
Extraction Extract(const std::filesystem::path& path,
                   std::size_t maxBytes = kMaxIndexedBytes,
                   std::stop_token token = {});

}  // namespace feathercast::file_content
