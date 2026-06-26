#pragma once

#include <stdint.h>

#ifdef _WIN32
#define FEATHERCAST_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
#define FEATHERCAST_EXTENSION_EXPORT extern "C"
#endif

#define FEATHERCAST_EXTENSION_API_VERSION_1 1u
#define FEATHERCAST_EXTENSION_API_VERSION_2 2u
#define FEATHERCAST_EXTENSION_API_VERSION FEATHERCAST_EXTENSION_API_VERSION_2

typedef uint32_t (*FeatherCastExtensionApiVersionFn)();
typedef uint32_t (*FeatherCastExtensionHandleJsonFn)(const char* requestUtf8,
                                                  char* responseUtf8,
                                                  uint32_t responseCapacity);

