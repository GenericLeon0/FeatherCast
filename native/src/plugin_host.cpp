#include "IExtension.h"
#include "extension_protocol.hpp"

#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using ApiVersionFn = uint32_t (*)();
using HandleJsonFn = uint32_t (*)(const char*, char*, uint32_t);

uint32_t CallPluginSeh(HandleJsonFn fn, const char* request, char* response, uint32_t capacity) {
  __try {
    return fn(request, response, capacity);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

uint32_t CallPlugin(HandleJsonFn fn, const char* request, char* response, uint32_t capacity) {
  try {
    return CallPluginSeh(fn, request, response, capacity);
  } catch (...) {
    return 0;
  }
}

std::string ErrorJson(const char* code) {
  return std::string("{\"error\":\"") + code + "\"}";
}

std::string AdaptRequestForPluginApi(std::string request, uint32_t pluginApiVersion) {
  if (pluginApiVersion == feathercast::extensions::kApiVersion) return request;

  const std::string marker = "\"apiVersion\"";
  size_t pos = request.find(marker);
  if (pos == std::string::npos) return request;
  pos = request.find(':', pos + marker.size());
  if (pos == std::string::npos) return request;
  ++pos;
  while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t')) ++pos;
  const size_t start = pos;
  while (pos < request.size() && request[pos] >= '0' && request[pos] <= '9') ++pos;
  if (start == pos) return request;
  request.replace(start, pos - start, std::to_string(pluginApiVersion));
  return request;
}

std::string HandleRequest(HandleJsonFn fn, uint32_t pluginApiVersion, const std::string& request) {
  const std::string adaptedRequest = AdaptRequestForPluginApi(request, pluginApiVersion);
  std::vector<char> buffer(4096, '\0');
  uint32_t required = CallPlugin(fn, adaptedRequest.c_str(), buffer.data(), static_cast<uint32_t>(buffer.size()));
  if (!feathercast::extensions::ResponseSizeAllowed(required)) return ErrorJson("plugin-call-failed");

  if (required > buffer.size()) {
    buffer.assign(required, '\0');
    required = CallPlugin(fn, adaptedRequest.c_str(), buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (!feathercast::extensions::ResponseSizeAllowed(required) || required > buffer.size()) {
      return ErrorJson("plugin-call-failed");
    }
  }

  const size_t length = strnlen_s(buffer.data(), buffer.size());
  if (length >= buffer.size()) return ErrorJson("plugin-call-failed");
  return std::string(buffer.data(), length);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2 || !argv[1] || !argv[1][0]) return 2;

  HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) return 3;

  auto apiVersion = reinterpret_cast<ApiVersionFn>(GetProcAddress(dll, "FeatherCastExtensionApiVersion"));
  auto handleJson = reinterpret_cast<HandleJsonFn>(GetProcAddress(dll, "FeatherCastExtensionHandleJson"));
  if (!apiVersion || !handleJson) {
    FreeLibrary(dll);
    return 4;
  }

  const uint32_t pluginApiVersion = apiVersion();
  if (pluginApiVersion < feathercast::extensions::kMinSupportedApiVersion ||
      pluginApiVersion > feathercast::extensions::kApiVersion) {
    FreeLibrary(dll);
    return 5;
  }

  std::ios::sync_with_stdio(false);
  std::string request;
  while (std::getline(std::cin, request)) {
    if (!request.empty() && request.back() == '\r') request.pop_back();
    if (request.empty()) {
      std::cout << ErrorJson("empty-request") << '\n' << std::flush;
      continue;
    }
    std::cout << HandleRequest(handleJson, pluginApiVersion, request) << '\n' << std::flush;
  }

  FreeLibrary(dll);
  return 0;
}

