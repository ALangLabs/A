#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinValues(const std::string& name, CallExpr& call) {

  if (name == "py_buffer") {
    // Zero-copy view over a Python object's buffer protocol; the result is
    // a dynamic value (indexing/len/iteration dispatch at runtime).
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    inferExpr(*call.args[0].value);
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Var;
    t->allowUnresolved = true;
    return t;
  }

  // Structured values (Json) are opaque; any non-void value may be serialized.

  if (name == "len") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::String || resolved->kind == TypeKind::Array ||
        resolved->kind == TypeKind::Map || resolved->kind == TypeKind::Set) {
      return typeInt(64, true);
    }
    if (resolved->kind == TypeKind::Var) {
      /* dynamic: inferred at runtime */ (void)0;
    } else {
      sink_.error(call.args[0].value->span,
                  "'len' cannot be applied to " + typePhrase(resolved) + ".",
                  "'len' works on strings, arrays, maps, and sets.");
    }
    return typeInt(64, true);
  }

  if (name == "set") {
    if (call.args.size() > 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeSet(typeError());
    }
    if (call.args.empty()) {
      return typeSet(freshVar(Constraint::None, call.span, "the element type of an empty set"));
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Set) return type;
    if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Range) {
      return typeSet(resolved->elem);
    }
    if (resolved->kind == TypeKind::String) return typeSet(typeChar());
    if (resolved->kind == TypeKind::Var) {
      return typeSet(freshVar(Constraint::None, call.span, "the element type of 'set'"));
    }
    sink_.error(call.args[0].value->span,
                "'set' requires an array, range, set, or string, found " + typePhrase(resolved) +
                    ".");
    return typeSet(typeError());
  }

  if (name == "move") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot move a void value.");
    }
    return type;
  }

  if (name == "copy") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot copy a void value.");
      return typeError();
    }
    if (resolved->kind == TypeKind::Var) {
      /* dynamic: inferred at runtime */ (void)0;
    }
    return type;
  }

  if (name == "borrow" || name == "borrow_mut") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeError();
    }
    if (call.args[0].value->kind != ExprKind::Ident) {
      sink_.error(call.args[0].value->span,
                  "The argument of '" + name + "' must be a variable.",
                  "Borrows are tied to a binding so the analyzer can track its lifetime.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::Void || resolved->kind == TypeKind::Int ||
        resolved->kind == TypeKind::Float || resolved->kind == TypeKind::Bool ||
        resolved->kind == TypeKind::Char) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a reference-like value, found " + typePhrase(resolved) +
                      ".",
                  "Borrowing applies to arrays, maps, sets, strings, and structs.");
      return typeError();
    }
    return type;
  }

  if (name == "str") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot convert a void value to a string.");
    }
    return typeString();
  }

  if (name == "Some") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeOptional(typeError());
    }
    TypePtr value = inferExpr(*call.args[0].value);
    if (resolveType(value)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot wrap a void value in 'Some'.");
    }
    return typeOptional(value);
  }

  if (name == "Ok") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
      e->allowUnresolved = true;
      return typeResult(typeError(), e);
    }
    TypePtr value = inferExpr(*call.args[0].value);
    TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
    e->allowUnresolved = true;
    return typeResult(value, e);
  }

  if (name == "Err") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
      t->allowUnresolved = true;
      return typeResult(t, typeError());
    }
    TypePtr error = inferExpr(*call.args[0].value);
    TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
    t->allowUnresolved = true;
    return typeResult(t, error);
  }

  return nullptr;
}

}  // namespace a
