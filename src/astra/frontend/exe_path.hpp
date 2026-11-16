#pragma once
// Absolute path of the running executable, used to locate the staged Astra
// backend and stdlib relative to the installed `a`/`a-lsp` binaries.
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <climits>
#  include <cstdlib>
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

namespace a {

inline std::string exePath() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  return std::string(buf, n);
#elif defined(__APPLE__)
  char buf[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
  char resolved[PATH_MAX];
  if (::realpath(buf, resolved) != nullptr) return std::string(resolved);
  return std::string(buf);
#else
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return std::string();
  buf[n] = '\0';
  return std::string(buf);
#endif
}

}  // namespace a
