#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace feathercast::network {

std::optional<std::string> HttpsGet(const std::wstring& host,
                                    const std::wstring& path);
std::optional<std::string> HttpsGetUrl(
    const std::wstring& url, std::size_t maxBytes = 2 * 1024 * 1024);
bool HttpsDownloadToFile(
    const std::wstring& url, const std::filesystem::path& destination,
    std::stop_token stopToken, std::size_t maxBytes = 250 * 1024 * 1024);

}  // namespace feathercast::network
