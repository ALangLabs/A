#pragma once

#include "lexer/lexer.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

namespace a {

inline const std::unordered_map<std::string, Tok>& keywordTable() {
  static const std::unordered_map<std::string, Tok> table = {
      {"fn", Tok::KwFn},         {"if", Tok::KwIf},
      {"elif", Tok::KwElif},     {"else", Tok::KwElse},     {"while", Tok::KwWhile},
      {"for", Tok::KwFor},       {"in", Tok::KwIn},         {"loop", Tok::KwLoop},
      {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
      {"return", Tok::KwReturn}, {"defer", Tok::KwDefer},
      {"class", Tok::KwClass},   {"metaclass", Tok::KwMetaclass},
      {"pass", Tok::KwPass},
      {"self", Tok::KwSelf},     {"true", Tok::KwTrue},     {"false", Tok::KwFalse},
      {"and", Tok::KwAnd},       {"or", Tok::KwOr},         {"not", Tok::KwNot},
      {"import", Tok::KwImport}, {"from", Tok::KwFrom},       {"as", Tok::KwAs},
      {"match", Tok::KwMatch},   {"None", Tok::KwNone},
       {"enum", Tok::KwEnum},     {"do", Tok::KwDo},     {"type", Tok::KwType},
      {"trait", Tok::KwTrait},   {"where", Tok::KwWhere}, {"comptime", Tok::KwComptime},
      {"impl", Tok::KwImpl},   {"is", Tok::KwIs},   {"del", Tok::KwDel},
  };
  return table;
}

inline bool isIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

inline bool isIdentContinue(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

}  // namespace a
