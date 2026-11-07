#pragma once

#include "tooling/docgen.hpp"

namespace a::tooling {

inline const char* kindName(DocItem::Kind kind) {
  switch (kind) {
    case DocItem::Kind::Function: return "function";
    case DocItem::Kind::Class: return "class";
    case DocItem::Kind::Struct: return "struct";
    case DocItem::Kind::Metaclass: return "metaclass";
    case DocItem::Kind::Enum: return "enum";
    case DocItem::Kind::Trait: return "trait";
    case DocItem::Kind::TypeAlias: return "type";
    case DocItem::Kind::Constant: return "constant";
    case DocItem::Kind::Variable: return "variable";
  }
  return "item";
}

}  // namespace a::tooling
