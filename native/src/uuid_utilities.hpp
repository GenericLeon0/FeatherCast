#pragma once

#include <windows.h>

#include <array>
#include <cwchar>
#include <string>

namespace feathercast::uuid_utilities {

inline std::wstring Format(const GUID& guid) {
  std::array<wchar_t, 37> buffer{};
  swprintf_s(buffer.data(), buffer.size(),
             L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
             guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
             guid.Data4[6], guid.Data4[7]);
  return buffer.data();
}

}  // namespace feathercast::uuid_utilities
