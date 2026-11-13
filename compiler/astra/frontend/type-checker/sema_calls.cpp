#include "type-checker/sema_internal.hpp"

namespace a {
TypePtr Sema::checkMethodCall(const TypePtr& objectType, FieldExpr& field, CallExpr& call) {
  TypePtr object = resolveType(objectType);
  const std::string& method = field.field;

  auto rejectNamed = [&]() {
    for (auto& arg : call.args) {
      if (!arg.name.empty()) {
        sink_.error(arg.nameSpan, "Named arguments are not supported in method calls.");
        return true;
      }
    }
    return false;
  };

  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */
    for (auto& arg : call.args) inferExpr(*arg.value);
    auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }

  // Fixed-width SIMD vectors: lane reductions and conversions. These are
  // compiler intrinsics lowered to native <N x ty> IR by the LLVM backend.
  if (object->kind == TypeKind::Vector) {
    if (rejectNamed()) return typeError();
    auto noArgs = [&](const char* usage) {
      if (call.args.size() != 0) {
        sink_.error(call.span,
                    "Method '" + method + "' expects " + usage +
                        ", found " + std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    if (method == "sum" || method == "min" || method == "max") {
      if (!noArgs("no arguments")) return typeError();
      return object->elem;
    }
    if (method == "to_list") {
      if (!noArgs("no arguments")) return typeError();
      return typeArray(object->elem);
    }
    if (method == "dot") {
      if (call.args.size() != 1) {
        sink_.error(call.span,
                    "Method 'dot' expects 1 argument (vector), found " +
                        std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      TypePtr arg = resolveType(argType);
      if (!arg || arg->kind != TypeKind::Vector ||
          arg->length != object->length) {
        sink_.error(call.args[0].value->span,
                    "'dot' expects a vector[" + typeName(object->elem) + ", " +
                        std::to_string(object->length) + "].");
        return typeError();
      }
      if (!unify(arg->elem, object->elem)) {
        reportMismatch(argType, object, call.args[0].value->span, "in 'dot'");
        return typeError();
      }
      return object->elem;
    }
    sink_.error(field.fieldSpan,
                "vector[" + typeName(object->elem) + ", " +
                    std::to_string(object->length) +
                    "] has no method '" + method + "'.",
                "Vector methods: sum(), min(), max(), dot(other), to_list().");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    // Generic classes instantiate per type-argument combination: the stored
    // field/method types reference the class's shared type-parameter vars, so
    // each use must substitute this object's actual type arguments (and fresh
    // vars for any type parameters the member declares itself) before
    // checking. Without this the first call site would bind the shared vars
    // for every other instantiation in the module.
    const StructSym& structSym = structIt->second;
    auto memberMapping = [&]() {
      std::unordered_map<TypePtr, TypePtr> mapping;
      for (std::size_t i = 0; i < structSym.typeParamVars.size(); ++i) {
        if (i < object->params.size()) {
          mapping[structSym.typeParamVars[i]] = object->params[i];
        } else {
          TypePtr fresh = freshVar(Constraint::None, structSym.typeParamVars[i]->varSpan,
                                   "type parameter '" + structSym.typeParams[i] + "'");
          fresh->allowUnresolved = true;
          mapping[structSym.typeParamVars[i]] = fresh;
        }
      }
      return mapping;
    };
    auto methodIt = structIt->second.methods.find(method);
    if (methodIt == structIt->second.methods.end()) {
      for (const auto& fieldEntry : structIt->second.fields) {
        if (fieldEntry.first != method) continue;
        TypePtr fieldType = resolveType(fieldEntry.second);
        if (fieldType->kind == TypeKind::Function) {
          if (!structSym.typeParamVars.empty()) {
            return checkFunctionValueCall(method, instantiateType(fieldType, memberMapping()),
                                          call);
          }
          return checkFunctionValueCall(method, fieldEntry.second, call);
        }
        sink_.error(field.fieldSpan,
                    "Field '" + method + "' of '" + object->structName +
                        "' is not callable.",
                    "Only fields with a function type can be called.");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      sink_.error(field.fieldSpan,
                  "Struct '" + object->structName + "' has no method named '" + method + "'.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    if (rejectNamed()) return typeError();
    // Instantiate the stored signature for this object's type arguments so
    // each generic-class instantiation is checked independently (see the
    // memberMapping comment above). The method's own type parameters (those
    // beyond the class's) get fresh vars per call, like generic functions.
    FnSig effective = methodIt->second;
    if (!effective.typeParamVars.empty()) {
      auto mapping = memberMapping();
      for (std::size_t i = structSym.typeParamVars.size();
           i < effective.typeParamVars.size(); ++i) {
        TypePtr var = effective.typeParamVars[i];
        TypePtr fresh = freshVar(var->constraint, var->varSpan,
                                 var->varDescription.empty() ? "type parameter"
                                                             : var->varDescription,
                                 var->allowDefault);
        fresh->allowUnresolved = var->allowUnresolved;
        mapping[var] = fresh;
      }
      std::vector<TypePtr> params;
      params.reserve(effective.paramTypes.size());
      for (const auto& p : effective.paramTypes)
        params.push_back(instantiateType(p, mapping));
      effective.paramTypes = std::move(params);
      effective.retType = instantiateType(effective.retType, mapping);
    }
    const FnSig& sig = effective;
    std::size_t total = sig.paramTypes.size();
    std::size_t expected = total - 1;  // exclude self
    // self is always required, so user-facing required count is requiredArgs()-1.
    std::size_t required = sig.requiredArgs() > 0 ? sig.requiredArgs() - 1 : 0;
    bool arityOk = call.args.size() >= required &&
                   (sig.variadic || call.args.size() <= expected);
    if (!arityOk) {
      std::string expect;
      if (sig.variadic) {
        expect = required == 0 ? "any number of argument(s)"
                               : "at least " + std::to_string(required) + " argument(s)";
      } else if (required == expected) {
        expect = std::to_string(expected) + " argument(s)";
      } else {
        expect = "between " + std::to_string(required) + " and " +
                 std::to_string(expected) + " argument(s)";
      }
      sink_.error(call.span,
                  "Method '" + method + "' expects " + expect +
                      ", found " + std::to_string(call.args.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    TypePtr varElem;
    if (sig.variadic && total > 0) {
      TypePtr rt = resolveType(sig.paramTypes.back());
      if (rt && rt->kind == TypeKind::Array) varElem = rt->elem;
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      if (sig.variadic && i + 2 >= total) {
        if (varElem) {
          checkArgAgainst(*call.args[i].value, varElem,
                          "in variadic argument " + std::to_string(i + 1) + " to method '" + method + "'");
        } else {
          inferExpr(*call.args[i].value);
        }
        continue;
      }
      checkArgAgainst(*call.args[i].value, sig.paramTypes[i + 1],
                      "in argument " + std::to_string(i + 1) + " to method '" + method + "'");
    }
    return sig.retType;
  }

  if (object->kind == TypeKind::Array) {
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto elementArg = [&](std::size_t index, const std::string& where) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, object->elem)) {
        reportMismatch(type, object->elem, call.args[index].value->span, where);
      }
    };
    auto indexArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      requireInteger(type, call.args[index].value->span, what);
    };
    auto rejectResize = [&](const char* what) {
      if (object->length < 0) return false;
      sink_.error(field.fieldSpan,
                  "'" + method + "' cannot resize a fixed-size array of type " +
                      typeName(object) + ".",
                  std::string(what) + " Use a dynamically sized array ('[T]') when the length "
                  "must change.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return true;
    };

    if (method == "append") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      if (rejectResize("Appending would grow the array.")) return typeVoid();
      elementArg(0, "in 'append'");
      return typeVoid();
    }
    if (method == "extend") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (array)")) return typeError();
      if (rejectResize("Extending would grow the array.")) return typeVoid();
      TypePtr argType = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(argType);
      if (resolved->kind != TypeKind::Array) {
        sink_.error(call.args[0].value->span,
                    "'extend' expects an array, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, object->elem)) {
        reportMismatch(resolved->elem, object->elem, call.args[0].value->span, "in 'extend'");
      }
      return typeVoid();
    }
    if (method == "insert") {
      if (rejectNamed()) return typeError();
      if (!arity(2, 2, "2 arguments (index, value)")) return typeError();
      if (rejectResize("Inserting would grow the array.")) return typeVoid();
      indexArg(0, "The index passed to 'insert'");
      elementArg(1, "in 'insert'");
      return typeVoid();
    }
    if (method == "pop") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 1, "at most 1 argument (index)")) return typeError();
      if (rejectResize("Popping would shrink the array.")) return object->elem;
      if (call.args.size() == 1) indexArg(0, "The index passed to 'pop'");
      return object->elem;
    }
    if (method == "remove") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      if (rejectResize("Removing would shrink the array.")) return typeVoid();
      elementArg(0, "in 'remove'");
      return typeVoid();
    }
    if (method == "clear" || method == "reverse" || method == "sort") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      if (method == "clear" && rejectResize("Clearing would shrink the array.")) return typeVoid();
      if (method == "sort") {
        TypePtr element = resolveType(object->elem);
        if (element->kind != TypeKind::Var && element->kind != TypeKind::Int &&
            element->kind != TypeKind::Float && element->kind != TypeKind::String) {
          sink_.error(field.fieldSpan,
                      "'sort' requires an array of integers, floats, or strings, found " +
                          typePhrase(object) + ".");
        }
      }
      return typeVoid();
    }
    if (method == "copy") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(object->elem, object->length);
    }
    if (method == "count") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0, "in 'count'");
      return typeInt(64, true);
    }
    if (method == "index") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 3, "1 to 3 arguments (value[, start[, end]])")) return typeError();
      elementArg(0, "in 'index'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        indexArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeInt(64, true);
    }
    if (method == "contains") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0, "in 'contains'");
      return typeBool();
    }
    if (method == "join") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (separator)")) return typeError();
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, typeString())) {
        reportMismatch(argType, typeString(), call.args[0].value->span,
                       "in separator argument to 'join'");
      }
      return typeString();
    }
    if (method == "remove_at") {
      if (rejectNamed()) return typeError();
      if (!arity(1, 1, "1 argument (index)")) return typeError();
      if (rejectResize("Removing would shrink the array.")) return object->elem;
      indexArg(0, "The index passed to 'remove_at'");
      return object->elem;
    }
    if (method == "first" || method == "last") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeOptional(object->elem);
    }
    if (method == "len") {
      if (rejectNamed()) return typeError();
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeInt(64, true);
    }
    sink_.error(field.fieldSpan,
                "Array has no method named '" + method + "'.",
                availableMethodsHelp("array", arrayMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Map) {
    if (rejectNamed()) return typeError();
    TypePtr key = object->elem;
    TypePtr value = object->params.empty() ? typeError() : object->params[0];
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto keyArg = [&](std::size_t index) {
      checkArgAgainst(*call.args[index].value, key, "in the key argument to '" + method + "'");
    };

    if (method == "get") {
      if (!arity(1, 2, "1 or 2 arguments (key[, default])")) return typeError();
      keyArg(0);
      if (call.args.size() == 2) {
        checkArgAgainst(*call.args[1].value, value, "in the default argument to 'get'");
        return value;
      }
      return typeOptional(value);
    }
    if (method == "pop") {
      if (!arity(1, 2, "1 or 2 arguments (key[, default])")) return typeError();
      keyArg(0);
      if (call.args.size() == 2) {
        checkArgAgainst(*call.args[1].value, value, "in the default argument to 'pop'");
      }
      return value;
    }
    if (method == "remove_at") {
      if (!arity(1, 1, "1 argument (key)")) return typeError();
      keyArg(0);
      return value;
    }
    if (method == "setdefault") {
      if (!arity(1, 2, "1 or 2 arguments (key[, default])")) return typeError();
      keyArg(0);
      if (call.args.size() == 2) {
        checkArgAgainst(*call.args[1].value, value, "in the default argument to 'setdefault'");
      }
      return value;
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (key)")) return typeError();
      keyArg(0);
      return typeBool();
    }
    if (method == "keys") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(key);
    }
    if (method == "values") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(value);
    }
    if (method == "items") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(typeTuple({key, value}));
    }
    if (method == "clear") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeVoid();
    }
    if (method == "copy") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return object;
    }
    if (method == "update") {
      if (!arity(1, 1, "1 argument (map)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Map) {
        sink_.error(call.args[0].value->span,
                    "'update' expects a map, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved, object)) {
        sink_.error(call.args[0].value->span,
                    "'update' requires a map with the same key and value types as " +
                        typeName(object) + ".");
      }
      return typeVoid();
    }
    if (method == "len") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeInt(64, true);
    }
    sink_.error(field.fieldSpan,
                "Map has no method named '" + method + "'.",
                availableMethodsHelp("map", mapMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Set) {
    if (rejectNamed()) return typeError();
    TypePtr element = object->elem;
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto elementArg = [&](std::size_t index) {
      checkArgAgainst(*call.args[index].value, element, "in the argument to '" + method + "'");
    };

    if (method == "add") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeVoid();
    }
    if (method == "remove" || method == "discard") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeVoid();
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elementArg(0);
      return typeBool();
    }
    if (method == "clear") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeVoid();
    }
    if (method == "copy") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return object;
    }
    if (method == "union" || method == "intersection" || method == "difference") {
      if (!arity(1, 1, "1 argument (set)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Set) {
        sink_.error(call.args[0].value->span,
                    "'" + method + "' expects a set, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, element)) {
        reportMismatch(resolved->elem, element, call.args[0].value->span,
                       "in '" + method + "'");
      }
      return object;
    }
    if (method == "issubset" || method == "issuperset") {
      if (!arity(1, 1, "1 argument (set)")) return typeError();
      TypePtr other = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(other);
      if (resolved->kind != TypeKind::Set) {
        sink_.error(call.args[0].value->span,
                    "'" + method + "' expects a set, found " + typePhrase(resolved) + ".");
      }
      return typeBool();
    }
    if (method == "len") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeInt(64, true);
    }
    sink_.error(field.fieldSpan,
                "Set has no method named '" + method + "'.",
                availableMethodsHelp("set", setMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::String) {
    if (rejectNamed()) return typeError();
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    auto stringArg = [&](std::size_t index, const std::string& where) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[index].value->span, where);
      }
    };
    auto intArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      requireInteger(type, call.args[index].value->span, what);
    };
    auto boolArg = [&](std::size_t index, const char* what) {
      TypePtr type = inferExpr(*call.args[index].value);
      if (!unify(type, typeBool())) {
        reportMismatch(type, typeBool(), call.args[index].value->span, what);
      }
    };

    if (method == "capitalize" || method == "casefold" || method == "lower" ||
        method == "upper" || method == "swapcase" || method == "title") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeString();
    }
    if (method == "isalnum" || method == "isalpha" || method == "isascii" ||
        method == "isdecimal" || method == "isdigit" || method == "isidentifier" ||
        method == "islower" || method == "isnumeric" || method == "isprintable" ||
        method == "isspace" || method == "istitle" || method == "isupper") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeBool();
    }
    if (method == "center" || method == "ljust" || method == "rjust") {
      if (!arity(1, 2, "1 or 2 arguments (width[, fillchar])")) return typeError();
      intArg(0, "The width argument");
      if (call.args.size() == 2) stringArg(1, "in the fill character");
      return typeString();
    }
    if (method == "count" || method == "find" || method == "rfind" || method == "index" ||
        method == "rindex") {
      if (!arity(1, 3, "1 to 3 arguments (sub[, start[, end]])")) return typeError();
      stringArg(0, "in argument 1 to '" + method + "'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        intArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeInt(64, true);
    }
    if (method == "startswith" || method == "endswith") {
      if (!arity(1, 3, "1 to 3 arguments (affix[, start[, end]])")) return typeError();
      stringArg(0, "in argument 1 to '" + method + "'");
      for (std::size_t i = 1; i < call.args.size(); ++i) {
        intArg(i, i == 1 ? "The 'start' argument" : "The 'end' argument");
      }
      return typeBool();
    }
    if (method == "expandtabs") {
      if (!arity(0, 1, "at most 1 argument (tabsize)")) return typeError();
      if (call.args.size() == 1) intArg(0, "The tabsize argument");
      return typeString();
    }
    if (method == "strip" || method == "lstrip" || method == "rstrip") {
      if (!arity(0, 1, "at most 1 argument (characters)")) return typeError();
      if (call.args.size() == 1) stringArg(0, "in the characters argument");
      return typeString();
    }
    if (method == "partition" || method == "rpartition") {
      if (!arity(1, 1, "1 argument (separator)")) return typeError();
      stringArg(0, "in the separator argument");
      return typeTuple({typeString(), typeString(), typeString()});
    }
    if (method == "removeprefix" || method == "removesuffix") {
      if (!arity(1, 1, "1 argument (affix)")) return typeError();
      stringArg(0, "in the affix argument");
      return typeString();
    }
    if (method == "replace") {
      if (!arity(2, 3, "2 or 3 arguments (old, new[, count])")) return typeError();
      stringArg(0, "in the 'old' argument");
      stringArg(1, "in the 'new' argument");
      if (call.args.size() == 3) intArg(2, "The count passed to 'replace'");
      return typeString();
    }
    if (method == "split" || method == "rsplit") {
      if (!arity(0, 2, "at most 2 arguments (separator[, maxsplit])")) return typeError();
      if (call.args.size() == 1) {
        TypePtr first = inferExpr(*call.args[0].value);
        if (!unify(first, typeString())) {
          requireInteger(first, call.args[0].value->span, "The maxsplit argument");
        }
      } else if (call.args.size() == 2) {
        stringArg(0, "in the separator argument");
        intArg(1, "The maxsplit argument");
      }
      return typeArray(typeString());
    }
    if (method == "splitlines") {
      if (!arity(0, 1, "at most 1 argument (keepends)")) return typeError();
      if (call.args.size() == 1) boolArg(0, "The 'keepends' argument");
      return typeArray(typeString());
    }
    if (method == "join") {
      if (!arity(1, 1, "1 argument (array)")) return typeError();
      TypePtr argType = inferExpr(*call.args[0].value);
      TypePtr resolved = resolveType(argType);
      if (resolved->kind != TypeKind::Array) {
        sink_.error(call.args[0].value->span,
                    "'join' expects an array of strings, found " + typePhrase(resolved) + ".");
      } else if (!unify(resolved->elem, typeString())) {
        reportMismatch(resolved->elem, typeString(), call.args[0].value->span,
                       "in the array passed to 'join'");
      }
      return typeString();
    }
    if (method == "zfill") {
      if (!arity(1, 1, "1 argument (width)")) return typeError();
      intArg(0, "The width argument");
      return typeString();
    }
    if (method == "contains") {
      if (!arity(1, 1, "1 argument (substring)")) return typeError();
      stringArg(0, "in the 'contains' argument");
      return typeBool();
    }
    if (method == "repeat") {
      if (!arity(1, 1, "1 argument (count)")) return typeError();
      intArg(0, "The count passed to 'repeat'");
      return typeString();
    }
    if (method == "chars") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeArray(typeChar());
    }
    if (method == "len") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeInt(64, true);
    }
    if (method == "format") {
      // "{} {0} {name}".format(...) — positional fields only.
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeString();
    }
    sink_.error(field.fieldSpan,
                "String has no method named '" + method + "'.",
                availableMethodsHelp("string", stringMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Tuple) {
    // Tuples share the runtime's read-only sequence methods (T_TUPLE takes
    // the same a_method seq path as lists); mutators are list-only.
    if (rejectNamed()) return typeError();
    auto arity = [&](std::size_t low, std::size_t high, const std::string& usage) -> bool {
      if (call.args.size() < low || call.args.size() > high) {
        sink_.error(call.span, "Method '" + method + "' expects " + usage + ", found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    // Heterogeneous tuples have no single element type; element args are
    // inferred but not unified (runtime equality is by value).
    TypePtr elem = object->params.empty() ? typeError() : object->params[0];
    auto elemArg = [&](std::size_t index) { inferExpr(*call.args[index].value); };

    if (method == "contains") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elemArg(0);
      return typeBool();
    }
    if (method == "count" || method == "index") {
      if (!arity(1, 1, "1 argument (value)")) return typeError();
      elemArg(0);
      return typeInt(64, true);
    }
    if (method == "first" || method == "last") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeOptional(elem);
    }
    if (method == "join") {
      if (!arity(1, 1, "1 argument (separator)")) return typeError();
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, typeString())) {
        reportMismatch(argType, typeString(), call.args[0].value->span,
                       "in separator argument to 'join'");
      }
      return typeString();
    }
    if (method == "copy") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return object;
    }
    if (method == "len") {
      if (!arity(0, 0, "no arguments")) return typeError();
      return typeInt(64, true);
    }
    sink_.error(field.fieldSpan,
                "Tuple has no method named '" + method + "'.",
                availableMethodsHelp("tuple", tupleMethods()));
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  sink_.error(field.fieldSpan,
              "Values of type '" + typeName(object) + "' have no methods.",
              "Methods are defined on structs and on arrays and strings.");
  for (auto& arg : call.args) inferExpr(*arg.value);
  return typeError();
}

namespace {
ExprPtr icStringLit(const std::string& text, Span span) {
  auto lit = std::make_unique<StringLitExpr>();
  lit->span = span;
  StringPart part;
  part.isExpr = false;
  part.text = text;
  lit->parts.push_back(std::move(part));
  return lit;
}
}  // namespace

TypePtr Sema::checkIcCall(CallExpr& call) {
  // `ic(...)` desugars into a call to the prelude's `ic_impl`: the compiler
  // injects the call-site context string and one source label per argument
  // as hidden leading arguments, and the A implementation renders the
  // litprinter-style output. ic() -> ic_impl(ctx, []), ic(x, y) ->
  // ic_impl(ctxOrEmpty, [labels], x, y).
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "'ic' does not accept named arguments.");
      inferExpr(*arg.value);
      return typeError();
    }
  }

  bool allSimple = true;
  for (auto& arg : call.args) {
    if (!icExprIsSimple(*arg.value)) {
      allSimple = false;
      break;
    }
  }
  // Breadcrumbs (ic()) always carry context; inspections suppress it when
  // every argument is a bare name or literal.
  const std::string ctx =
      call.args.empty() || !allSimple ? icContextString(call.span) : "";

  std::vector<ExprPtr> labelExprs;
  labelExprs.reserve(call.args.size());
  std::unordered_map<int, std::unique_ptr<tooling::SourceText>> sources;
  for (auto& arg : call.args) {
    std::string label;
    const SourceFile* file = sink_.file(arg.value->span.fileId);
    if (file && arg.value->span.valid()) {
      auto& slot = sources[arg.value->span.fileId];
      if (!slot) slot = std::make_unique<tooling::SourceText>(file->text());
      label = collapseIcWhitespace(slot->slice(arg.value->span));
    }
    if (icExprIsBare(*arg.value)) label.clear();
    labelExprs.push_back(icStringLit(label, arg.value->span));
  }
  auto labels = std::make_unique<ArrayLitExpr>();
  labels->span = call.span;
  labels->elements = std::move(labelExprs);

  std::vector<CallArg> args;
  args.reserve(call.args.size() + 2);
  CallArg ctxArg;
  ctxArg.value = icStringLit(ctx, call.span);
  args.push_back(std::move(ctxArg));
  CallArg labelsArg;
  labelsArg.value = std::move(labels);
  args.push_back(std::move(labelsArg));
  for (auto& arg : call.args) args.push_back(std::move(arg));
  call.args = std::move(args);

  const FnDecl* impl = findPreludeFunction("ic_impl");
  if (!impl) {
    sink_.error(call.span,
                "The prelude does not provide 'ic_impl'; `ic` requires the standard library.");
    return typeError();
  }
  call.calleeFunction = impl;
  call.calleeResolved = true;
  auto sigIt = signatures_.find(impl);
  if (sigIt == signatures_.end()) return typeError();
  return checkFunctionCall(sigIt->second, call);
}

std::string Sema::icContextString(Span span) const {
  std::string file = debug_magic::sourceFileName(sink_, span);
  const std::size_t slash = file.find_last_of("/\\");
  if (slash != std::string::npos) file = file.substr(slash + 1);
  const std::string fn =
      debug_magic::functionDisplayName(currentFunction_ ? currentFunction_->name : std::string());
  return fn == "<module>" ? file + ":" + std::to_string(span.line)
                          : file + ":" + std::to_string(span.line) + " in " + fn + "()";
}

}  // namespace a
