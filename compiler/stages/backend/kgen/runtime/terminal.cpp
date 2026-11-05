#include "backend/kgen/runtime/terminal.hpp"

#include <cstdlib>
#include <cstring>

#ifdef A_TERMINAL_POSIX
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace a::kgen::terminal {
namespace {

#ifdef A_TERMINAL_POSIX
struct TermiosState {
  termios saved{};
  bool valid = false;
};
TermiosState g_state;
#endif

void appendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

bool isTty() {
#ifdef A_TERMINAL_POSIX
  return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#else
  return false;
#endif
}

bool rawMode(bool enable) {
#ifdef A_TERMINAL_POSIX
  if (enable) {
    if (g_state.valid) return true;  // already raw
    if (tcgetattr(STDIN_FILENO, &g_state.saved) != 0) return false;
    termios raw = g_state.saved;
    // Pure cbreak: no ISIG so Ctrl-C arrives as byte 0x03 and the REPL line
    // editor can treat it as "interrupt this input" like IPython does.
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    g_state.valid = true;
    return true;
  }
  if (g_state.valid) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_state.saved);
    g_state.valid = false;
  }
  return true;
#else
  (void)enable;
  return false;
#endif
}

bool rawActive() {
#ifdef A_TERMINAL_POSIX
  return g_state.valid;
#else
  return false;
#endif
}

int readChar(int timeoutMs, std::string& out) {
#ifdef A_TERMINAL_POSIX
  pollfd pfd{STDIN_FILENO, POLLIN, 0};
  int timeout = timeoutMs < 0 ? -1 : timeoutMs;
  int ready;
  do {
    ready = poll(&pfd, 1, timeout);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0) return ready == 0 ? 0 : -1;

  unsigned char first = 0;
  ssize_t got;
  do {
    got = ::read(STDIN_FILENO, &first, 1);
  } while (got < 0 && errno == EINTR);
  if (got <= 0) return -1;

  // Control bytes and ASCII pass through as single-byte "characters".
  if (first < 0x80) {
    out.push_back(static_cast<char>(first));
    return 1;
  }

  // Multi-byte UTF-8: collect continuation bytes.
  int needed = (first & 0xE0) == 0xC0 ? 1 : (first & 0xF0) == 0xE0 ? 2
               : (first & 0xF8) == 0xF0 ? 3 : 0;
  char32_t cp = first & (needed == 1 ? 0x1F : needed == 2 ? 0x0F : 0x07);
  for (int i = 0; i < needed; ++i) {
    unsigned char next = 0;
    ssize_t n;
    do {
      n = ::read(STDIN_FILENO, &next, 1);
    } while (n < 0 && errno == EINTR);
    if (n <= 0 || (next & 0xC0) != 0x80) {
      appendUtf8(out, 0xFFFD);
      return 1;
    }
    cp = (cp << 6) | (next & 0x3F);
  }
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  appendUtf8(out, cp);
  return 1;
#else
  (void)timeoutMs;
  (void)out;
  return -1;
#endif
}

bool writeOut(const std::string& text) {
#ifdef A_TERMINAL_POSIX
  std::size_t written = 0;
  while (written < text.size()) {
    ssize_t n = ::write(STDOUT_FILENO, text.data() + written, text.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    written += static_cast<std::size_t>(n);
  }
  return true;
#else
  (void)text;
  return false;
#endif
}

void flushOut() {
#ifdef A_TERMINAL_POSIX
  fsync(STDOUT_FILENO);
#endif
}

std::pair<int, int> size() {
#ifdef A_TERMINAL_POSIX
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return {ws.ws_col, ws.ws_row > 0 ? ws.ws_row : 24};
  }
#endif
  return {80, 24};
}

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string homeDir() {
  std::string home = env("HOME");
  if (!home.empty()) return home;
#ifdef A_TERMINAL_POSIX
  home = env("USERPROFILE");
  if (!home.empty()) return home;
#endif
  return std::string(".");
}

}  // namespace a::kgen::terminal
