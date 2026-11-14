#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinMemory(const std::string& name, CallExpr& call) {

  std::string linalg = name;
  if (name == "dot") linalg = "linalg_dot";
  else if (name == "matvec") linalg = "linalg_matvec";
  else if (name == "matvec_t") linalg = "linalg_matvec_t";
  else if (name == "matmul") linalg = "linalg_matmul";
  else if (name == "softmax") linalg = "linalg_softmax";
  else if (name == "attention") linalg = "linalg_attention";

  if (linalg == "linalg_dot" || linalg == "linalg_matvec" || linalg == "linalg_matvec_t" ||
      linalg == "linalg_matmul" || linalg == "linalg_softmax" || linalg == "linalg_attention") {
    std::size_t expected = 5;
    if (linalg == "linalg_dot") expected = 2;
    else if (linalg == "linalg_softmax") expected = 1;
    else if (linalg == "linalg_attention") expected = 6;
    if (call.args.size() != expected) {
      builtinArityError(name, call, expected);
      inferCallArgTypes(call);
      return linalg == "linalg_dot" ? typeFloat(64) : typeArray(typeFloat(64));
    }
    for (std::size_t i = 0; i < call.args.size(); i++) inferExpr(*call.args[i].value);
    return linalg == "linalg_dot" ? typeFloat(64) : typeArray(typeFloat(64));
  }

  if (name == "f32s") {
    if (call.args.size() != 3) {
      builtinArityError(name, call, 3);
      inferCallArgTypes(call);
      return typeArray(typeFloat(64));
    }
    TypePtr data = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(data);
    if (resolved->kind != TypeKind::String && resolved->kind != TypeKind::Bytes) {
      reportMismatch(data, typeString(), call.args[0].value->span,
                     "in the data passed to 'f32s'");
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The byte offset passed to 'f32s'");
    requireInteger(inferExpr(*call.args[2].value), call.args[2].value->span,
                   "The element count passed to 'f32s'");
    return typeArray(typeFloat(64));
  }

  if (name == "unsafe_get" || name == "unsafe_set") {
    const bool getting = name == "unsafe_get";
    const std::size_t expected = getting ? 2 : 3;
    if (call.args.size() != expected) {
      builtinArityError(name, call, expected);
      inferCallArgTypes(call);
      return typeError();
    }
    TypePtr sequence = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(sequence);
    const bool isString = resolved->kind == TypeKind::String;
    const bool isMap = resolved->kind == TypeKind::Map;
    const bool isSet = resolved->kind == TypeKind::Set;
    // The runtime implements get on lists, tuples, sets, dicts, and strings,
    // and set on lists, tuples, and dicts. Tuple element types are not
    // statically recoverable, so they stay unsupported here.
    const bool supported = resolved->kind == TypeKind::Array || isString ||
                           isMap || (getting && isSet);
    if (!supported) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' requires " +
                      std::string(getting ? "an array, map, set, or string"
                                          : "an array or map") +
                      ", found " + typePhrase(resolved) + ".",
                  getting ? "Unchecked access is supported only for arrays, maps, sets, and strings."
                          : "Unchecked writes are supported only for arrays and maps.");
      for (std::size_t i = 1; i < call.args.size(); ++i) inferExpr(*call.args[i].value);
      return typeError();
    }
    if (isMap) {
      // On a map the second argument is the key, not an index.
      TypePtr keyType = resolved->elem;
      TypePtr valueType =
          resolved->params.empty() ? typeError() : resolved->params[0];
      if (getting) {
        checkArgAgainst(*call.args[1].value, keyType, "in 'unsafe_get'");
        // The runtime yields None when the key is absent.
        return typeOptional(valueType);
      }
      checkArgAgainst(*call.args[1].value, keyType, "in 'unsafe_set'");
      checkArgAgainst(*call.args[2].value, valueType, "in 'unsafe_set'");
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The index passed to '" + name + "'");
    if (getting) {
      /* On a string, unsafe.get returns the raw stored byte (0-255) as an
         int — this is the only way to inspect non-UTF-8 file bytes. */
      if (isString) return typeInt(64, true);
      return resolved->elem;
    }
    if (isString || isSet) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_set' does not support " +
                      std::string(isString ? "strings" : "sets") + ".",
                  "Unchecked writes are supported only for arrays and maps.");
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
    // The runtime stores the value in a slot table and returns an opaque
    // 1-based handle (an int at runtime). Pointer<T> is the static view of
    // "a handle to a cell holding a T"; unsafe.load/store dereference it.
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typePointer(typeError());
    }
    TypePtr value = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(value);
    if (resolved->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span,
                  "'unsafe_address_of' cannot take the address of a void value.");
      return typePointer(typeError());
    }
    return typePointer(value);
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
