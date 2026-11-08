#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinMemory(const std::string& name, CallExpr& call) {

  if (name == "linalg_dot" || name == "linalg_matvec" || name == "linalg_matvec_t") {
    const std::size_t expected = name == "linalg_dot" ? 2 : 5;
    if (call.args.size() != expected) {
      builtinArityError(name, call, expected);
      inferCallArgTypes(call);
      return name == "linalg_dot" ? typeFloat() : typeArray(typeFloat());
    }
    for (std::size_t i = 0; i < call.args.size(); i++) inferExpr(*call.args[i].value);
    return name == "linalg_dot" ? typeFloat() : typeArray(typeFloat());
  }

  if (name == "unsafe_get" || name == "unsafe_set") {
    const std::size_t expected = name == "unsafe_get" ? 2 : 3;
    if (call.args.size() != expected) {
      builtinArityError(name, call, expected);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr sequence = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(sequence);
    const bool isString = resolved->kind == TypeKind::String;
    if (resolved->kind != TypeKind::Array && !isString) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires an array, found " + typePhrase(resolved) + ".",
                  "Unchecked access is supported only for arrays and array views.");
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return typeError();
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to '" + name + "'");
    if (name == "unsafe_get") {
      /* On a string, unsafe.get returns the raw stored byte (0-255) as an
         int — this is the only way to inspect non-UTF-8 file bytes. */
      if (isString) return typeInt(64, true);
      return resolved->elem;
    }
    if (isString) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_set' does not support strings.",
                  "Strings are immutable; unchecked writes are supported only for arrays.");
      inferExpr(*call.args[2].value);
      return typeVoid();
    }
    checkArgAgainst(*call.args[2].value, resolved->elem, "in 'unsafe_set'");
    return typeVoid();
  }

  if (name == "arena_new" || name == "arena_pool" || name == "arena_region") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeAllocator();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The allocator capacity");
    return typeAllocator();
  }

  if (name == "arena_alloc" || name == "arena_release") {
    const bool allocating = name == "arena_alloc";
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return allocating ? typeInt(64, true) : typeVoid();
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the first argument to '" + name + "'");
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   allocating ? "The allocation size" : "The allocation handle");
    return allocating ? typeInt(64, true) : typeVoid();
  }

  if (name == "arena_reset") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the argument to 'arena_reset'");
    }
    return typeVoid();
  }

  if (name == "arena_used" || name == "arena_total") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeInt(64, true);
    }
    TypePtr allocator = inferExpr(*call.args[0].value);
    if (resolveType(allocator)->kind != TypeKind::Allocator) {
      reportMismatch(allocator, typeAllocator(), call.args[0].value->span,
                     "in the argument to '" + name + "'");
    }
    return typeInt(64, true);
  }

  if (name == "unsafe_address_of") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typePointer(typeError());
    }
    TypePtr sequence = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(sequence);
    if (resolved->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_address_of' requires an array, found " + typePhrase(resolved) + ".");
      inferExpr(*call.args[1].value);
      return typePointer(typeError());
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to 'unsafe_address_of'");
    return typePointer(resolved->elem);
  }

  if (name == "unsafe_offset") {
    if (call.args.size() != 2) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typePointer(typeError());
    }
    TypePtr pointer = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(pointer);
    if (resolved->kind != TypeKind::Pointer) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_offset' requires a pointer, found " + typePhrase(resolved) + ".");
      inferExpr(*call.args[1].value);
      return typePointer(typeError());
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The offset passed to 'unsafe_offset'");
    return pointer;
  }

  if (name == "unsafe_load" || name == "unsafe_store") {
    const bool writing = name == "unsafe_store";
    if (call.args.size() != (writing ? 2u : 1u)) {
      builtinArityError(name, call, writing ? 2 : 1);
      inferCallArgTypes(call);
      return writing ? typeVoid() : typeError();
    }
    TypePtr pointer = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(pointer);
    if (resolved->kind != TypeKind::Pointer) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires a pointer, found " + typePhrase(resolved) + ".");
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return writing ? typeVoid() : typeError();
    }
    if (!writing) return resolved->elem;
    checkArgAgainst(*call.args[1].value, resolved->elem, "in 'unsafe_store'");
    return typeVoid();
  }

  return nullptr;
}

}  // namespace a
