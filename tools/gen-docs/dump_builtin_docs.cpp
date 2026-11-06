// Dumps the A builtin documentation tables as JSON so the reference docs
// can be generated from the single source of truth (the doc tables the
// type checker and language server use).
//
// Usage: dump_builtin_docs (JSON path or stdout when omitted)
//
// Build:
//   g++ -std=c++20 -I compiler/astra/frontend -I runtime \
//       tools/gen-docs/dump_builtin_docs.cpp -o dump_builtin_docs
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "name-resolution/builtin_docs.hpp"
#include "type-checker/method_signatures.hpp"

namespace {

std::string jsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

void writeMethod(std::ostream& out, const a::MethodSig& method, bool first) {
  if (!first) out << ",\n";
  out << "    {\"name\": \"" << jsonEscape(method.name)
      << "\", \"signature\": \"" << jsonEscape(method.signature)
      << "\", \"documentation\": \"" << jsonEscape(method.documentation)
      << "\", \"example\": \"" << jsonEscape(method.example ? method.example : "")
      << "\"}";
}

}  // namespace

int main(int argc, char** argv) {
  std::ostringstream stream;
  std::ostream& out = stream;
  if (argc > 1) {
    // Genuine output path: open later via ofstream below (kept simple).
  }

  std::vector<a::BuiltinDoc> docs;
  for (const auto& entry : a::builtinDocs()) {
    docs.push_back(entry.second);
  }
  std::sort(docs.begin(), docs.end(),
            [](const a::BuiltinDoc& x, const a::BuiltinDoc& y) {
              return std::string(x.name) < std::string(y.name);
            });

  std::ostringstream body;
  body << "{\n  \"builtins\": [\n";
  bool first = true;
  for (const a::BuiltinDoc& doc : docs) {
    if (!first) body << ",\n";
    first = false;
    body << "    {\"name\": \"" << jsonEscape(doc.name)
         << "\", \"signature\": \"" << jsonEscape(doc.signature)
         << "\", \"documentation\": \"" << jsonEscape(doc.documentation)
         << "\", \"example\": \"" << jsonEscape(doc.example ? doc.example : "")
         << "\", \"seeAlso\": \"" << jsonEscape(doc.seeAlso ? doc.seeAlso : "") << "\"}";
  }
  body << "\n  ],\n  \"methods\": {\n";
  const std::pair<const char*, const std::vector<a::MethodSig>*> tables[] = {
      {"array", &a::arrayMethods()},
      {"map", &a::mapMethods()},
      {"set", &a::setMethods()},
      {"string", &a::stringMethods()},
  };
  bool firstTable = true;
  for (const auto& [name, table] : tables) {
    if (!firstTable) body << ",\n";
    firstTable = false;
    body << "    \"" << jsonEscape(name) << "\": [\n";
    bool firstMethod = true;
    for (const a::MethodSig& method : *table) {
      writeMethod(body, method, firstMethod);
      firstMethod = false;
    }
    body << "\n    ]";
  }
  body << "\n  }\n}\n";

  if (argc > 1) {
    std::ofstream file(argv[1], std::ios::binary);
    if (!file) {
      std::cerr << "cannot open " << argv[1] << "\n";
      return 1;
    }
    file << body.str();
  } else {
    std::cout << body.str();
  }
  return 0;
}
