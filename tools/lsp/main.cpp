// Entry point for a-lsp: --version/--help handling and stdio setup, then
// the LanguageServer run loop (see server.cpp).

#include "server.hpp"

#include <iostream>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--version" || argument == "-V") {
      std::cout << "a-language-server " << a::lsp::kServerVersion << "\n";
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: a-lsp [--stdio]\n"
                   "\n"
                   "A language server for the A programming language. It speaks the\n"
                   "Language Server Protocol over stdin/stdout. Editors normally start it\n"
                   "automatically; pass --stdio when they require an explicit transport.\n";
      return 0;
    }
  }

  std::ios::sync_with_stdio(false);
  a::lsp::LanguageServer server;
  return server.run();
}
