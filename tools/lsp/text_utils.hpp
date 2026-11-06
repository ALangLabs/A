#pragma once

// Source-text and LSP position helpers shared by every handler: filesystem
// paths <-> file:// URIs, 1-based lines <-> UTF-16 columns, and the JSON
// builders for Position / Range / Location.

#include <array>
#include <string>
#include <vector>

#include "analysis.hpp"
#include "json.hpp"

namespace a::lsp {

using a::Span;
using jsonlite::Value;

std::string canonicalPath(const std::string& path);
std::string pathToUri(const std::string& path);
std::string uriToPath(const std::string& uri);
std::string lineText(const std::string& text, int oneBasedLine);
int byteToUtf16Column(const std::string& line, int zeroBasedByteColumn);
int utf16ToByteColumn(const std::string& line, int character);
Value positionValue(const std::string& text, Span span);
Value rangeValue(const std::string& text, Span span);
Value locationValue(const Compilation& compilation, const std::string& path, Span span);
Value symbolRange(const std::string& text, Span span);
std::vector<std::string> readStringArray(const Value* value);
std::string trimLine(const std::string& line);

// Myers O(ND) line diff: hunks [oldStart,oldEnd) -> [newStart,newEnd).
std::vector<std::array<int, 4>> diffLineHunks(const std::vector<std::string>& a,
                                              const std::vector<std::string>& b);

// Splits into lines keeping trailing '\n'; trailing newline yields a final
// empty part so EOF positions stay representable.
std::vector<std::string> splitLinesKeep(const std::string& text);

}  // namespace a::lsp
