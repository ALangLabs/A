#pragma once

// POSIX terminal primitives for the self-hosted A REPL (the `_pyrepl` analog).
// Only compiled on Unix-like platforms; Windows builds report "unavailable".

#include <string>
#include <utility>

namespace a::kgen::terminal {

#if defined(__unix__) || defined(__APPLE__)
#define A_TERMINAL_POSIX 1
#endif

bool isTty();

// Puts fd 0 into cbreak-style mode: per-character reads, no echo, no line
// buffering, no ISIG (so Ctrl-C arrives as byte 0x03 for the REPL to handle),
// no IXON flow control. Returns the previous state so it can be restored.
bool rawMode(bool enable);
bool rawActive();

// Reads one UTF-8 encoded character from stdin.
// Returns 1 and appends the encoded character to `out`, 0 on timeout,
// -1 on EOF/error. timeoutMs < 0 blocks forever.
int readChar(int timeoutMs, std::string& out);

bool writeOut(const std::string& text);
void flushOut();

// Terminal size as (columns, rows); falls back to 80x24.
std::pair<int, int> size();

std::string env(const char* name);
std::string homeDir();

}  // namespace a::kgen::terminal
