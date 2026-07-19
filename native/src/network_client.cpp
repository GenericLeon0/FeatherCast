#include "network_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <utility>
#include <vector>

namespace feathercast::network {
namespace {

template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
  ~ScopeExit() { fn_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  F fn_;
};

struct HttpsUrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

std::optional<HttpsUrlParts> ParseHttpsUrl(const std::wstring& url) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  std::wstring mutableUrl = url;
  if (!WinHttpCrackUrl(mutableUrl.c_str(), 0, 0, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      !components.lpszHostName || components.dwHostNameLength == 0) {
    return std::nullopt;
  }
  HttpsUrlParts parts;
  parts.host.assign(components.lpszHostName, components.dwHostNameLength);
  if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
    parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
    parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (parts.path.empty()) parts.path = L"/";
  parts.port = components.nPort ? components.nPort
                                : INTERNET_DEFAULT_HTTPS_PORT;
  return parts;
}

bool IsSuccessful(HINTERNET request) {
  DWORD status = 0;
  DWORD size = sizeof(status);
  const bool queried = WinHttpQueryHeaders(
                           request,
                           WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                           WINHTTP_NO_HEADER_INDEX) != FALSE;
  return IsSuccessfulStatusQuery(queried, status);
}

}  // namespace

std::optional<std::string> HttpsGet(const std::wstring& host,
                                    const std::wstring& path) {
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return std::nullopt;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET connect = WinHttpConnect(
      session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) return std::nullopt;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return std::nullopt;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  static constexpr wchar_t kGitHubHeaders[] =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28";
  WinHttpAddRequestHeaders(
      request, kGitHubHeaders, static_cast<DWORD>(-1),
      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return std::nullopt;
  }
  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) {
      break;
    }
    chunk.resize(read);
    body += chunk;
    if (body.size() > 2 * 1024 * 1024) break;
  }
  return body;
}

std::optional<std::string> HttpsGetUrl(const std::wstring& url,
                                       std::size_t maxBytes) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return std::nullopt;
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return std::nullopt;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET connect = WinHttpConnect(
      session, parts->host.c_str(), parts->port, 0);
  if (!connect) return std::nullopt;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", parts->path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return std::nullopt;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return std::nullopt;
  }
  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    if (body.size() + available > maxBytes) return std::nullopt;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) {
      return std::nullopt;
    }
    chunk.resize(read);
    body += chunk;
  }
  return body;
}

bool HttpsDownloadToFile(const std::wstring& url,
                         const std::filesystem::path& destination,
                         std::stop_token stopToken, std::size_t maxBytes) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return false;
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  auto temp = destination;
  temp += L".tmp";
  std::filesystem::remove(temp, ec);
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
  HINTERNET connect = WinHttpConnect(
      session, parts->host.c_str(), parts->port, 0);
  if (!connect) return false;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", parts->path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return false;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return false;
  }
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  std::size_t total = 0;
  DWORD available = 0;
  while (!stopToken.stop_requested() &&
         WinHttpQueryDataAvailable(request, &available) && available > 0) {
    if (total + available > maxBytes) return false;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) return false;
    if (read == 0) continue;
    file.write(chunk.data(), static_cast<std::streamsize>(read));
    if (!file) return false;
    total += read;
  }
  file.close();
  if (stopToken.stop_requested() || total == 0) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  if (!MoveFileExW(temp.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(temp, destination, ec);
  }
  const bool ok = std::filesystem::exists(destination, ec);
  if (!ok) std::filesystem::remove(temp, ec);
  return ok;
}

}  // namespace feathercast::network
