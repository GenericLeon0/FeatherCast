#include "IExtension.h"

#include <windows.h>

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4297)
#endif

namespace {

uint32_t WriteResponse(const std::string& response, char* buffer, uint32_t capacity) {
  const uint32_t required = static_cast<uint32_t>(response.size() + 1);
  if (!buffer || capacity < required) return required;
  memcpy(buffer, response.c_str(), required);
  return required;
}

bool Contains(const char* text, const char* needle) {
  return text && std::string(text).find(needle) != std::string::npos;
}

}  // namespace

LEANCAST_EXTENSION_EXPORT uint32_t LeanCastExtensionApiVersion() {
#ifdef LEANCAST_TEST_BAD_API
  return 999;
#else
  return LEANCAST_EXTENSION_API_VERSION;
#endif
}

LEANCAST_EXTENSION_EXPORT uint32_t LeanCastExtensionHandleJson(const char* requestUtf8,
                                                               char* responseUtf8,
                                                               uint32_t responseCapacity) {
  if (Contains(requestUtf8, "\"query\":\"throw\"")) {
    throw std::runtime_error("test exception");
  }
  if (Contains(requestUtf8, "\"query\":\"slow\"")) {
    Sleep(1000);
    return WriteResponse("{\"items\":[]}", responseUtf8, responseCapacity);
  }
  if (Contains(requestUtf8, "\"query\":\"huge\"")) {
    return 1024 * 1024 + 2;
  }
  if (Contains(requestUtf8, "\"query\":\"malformed\"")) {
    return WriteResponse("{\"items\":[", responseUtf8, responseCapacity);
  }
  if (Contains(requestUtf8, "\"type\":\"activate\"")) {
    return WriteResponse(
        "{\"handled\":true,\"closeOverlay\":true,"
        "\"action\":{\"type\":\"copyText\",\"value\":\"activated\"}}",
        responseUtf8, responseCapacity);
  }
  return WriteResponse(
      "{\"items\":[{\"id\":\"demo\",\"title\":\"Demo Result\","
      "\"subtitle\":\"Test extension\",\"keywords\":[\"demo\",\"test\"],"
      "\"score\":7,\"payload\":{\"token\":\"abc\"}}]}",
      responseUtf8, responseCapacity);
}
