#pragma once

#include <stdint.h>

#ifdef _WIN32
#define LEANCAST_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
#define LEANCAST_EXTENSION_EXPORT extern "C"
#endif

#define LEANCAST_EXTENSION_API_VERSION 1u

typedef uint32_t (*LeanCastExtensionApiVersionFn)();
typedef uint32_t (*LeanCastExtensionHandleJsonFn)(const char* requestUtf8,
                                                  char* responseUtf8,
                                                  uint32_t responseCapacity);

