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

std::string HandleRequest(HandleJsonFn fn, const std::string& request) {
  std::vector<char> buffer(4096, '\0');
  uint32_t required = CallPlugin(fn, request.c_str(), buffer.data(), static_cast<uint32_t>(buffer.size()));
  if (!leancast::extensions::ResponseSizeAllowed(required)) return ErrorJson("plugin-call-failed");

  if (required > buffer.size()) {
    buffer.assign(required, '\0');
    required = CallPlugin(fn, request.c_str(), buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (!leancast::extensions::ResponseSizeAllowed(required) || required > buffer.size()) {
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

  auto apiVersion = reinterpret_cast<ApiVersionFn>(GetProcAddress(dll, "LeanCastExtensionApiVersion"));
  auto handleJson = reinterpret_cast<HandleJsonFn>(GetProcAddress(dll, "LeanCastExtensionHandleJson"));
  if (!apiVersion || !handleJson) {
    FreeLibrary(dll);
    return 4;
  }

  if (apiVersion() != LEANCAST_EXTENSION_API_VERSION) {
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
    std::cout << HandleRequest(handleJson, request) << '\n' << std::flush;
  }

  FreeLibrary(dll);
  return 0;
}

