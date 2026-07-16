#pragma once

#include <cstdio>
#include <cstdlib>

namespace feathercast::test {

[[noreturn]] inline void Fail(const char* expression, const char* file, int line) {
  std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace feathercast::test

// The existing suites historically used assert(), which disappears in Release
// builds. Keep the call sites readable while making every check unconditional.
#ifdef assert
#undef assert
#endif
#define assert(expression) \
  ((expression) ? static_cast<void>(0) : ::feathercast::test::Fail(#expression, __FILE__, __LINE__))
