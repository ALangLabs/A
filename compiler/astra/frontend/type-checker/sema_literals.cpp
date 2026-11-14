#include "type-checker/sema_internal.hpp"

namespace a {
TypePtr Sema::inferIndex(IndexExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr indexType = inferExpr(*expr.index);

  TypePtr object = resolveType(objectType);
  if (object->kind == TypeKind::Map && !object->params.empty()) {
    if (!unify(indexType, object->elem)) {
      reportMismatch(indexType, object->elem, expr.index->span, "in the map key");
    }
    return object->params[0];
  }
  if (object->kind == TypeKind::Map) return typeError();
  // Dynamic: index type checked at runtime (maps use any hashable key).
  if (object->kind == TypeKind::Array) return object->elem;
  if (object->kind == TypeKind::String) return typeChar();
  // Bytes index yields the byte value (i64 0..255), like Python's bytearray.
  if (object->kind == TypeKind::Bytes) return typeInt(64, true);
  // Vector lane access yields the element type; the index must be an integer.
  if (object->kind == TypeKind::Vector) {
    requireInteger(indexType, expr.index->span, "The vector index");
    return object->elem;
  }
  // Tuples are indexable when the element position is known at compile time;
  // a literal index selects that element's exact type, otherwise the result is
  // dynamically typed (still bounds-checked at run time).
  if (object->kind == TypeKind::Tuple) {
    if (auto* lit = dynamic_cast<IntLitExpr*>(expr.index.get())) {
      std::int64_t i = lit->value;
      if (i < 0) i += static_cast<std::int64_t>(object->params.size());
      if (i >= 0 && i < static_cast<std::int64_t>(object->params.size()))
        return object->params[static_cast<std::size_t>(i)];
    }
    if (!object->params.empty()) {
      bool same = true;
      for (const auto& p : object->params)
        if (resolveType(p)->kind != resolveType(object->params[0])->kind) { same = false; break; }
      if (same) return object->params[0];
    }
    auto t = std::make_shared<Type>(); t->kind = TypeKind::Var; t->allowUnresolved = true; return t;
  }
  if (object->kind == TypeKind::Var || object->kind == TypeKind::Union ||
      object->kind == TypeKind::Any) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  if (object->kind == TypeKind::Set) {
    sink_.error(expr.object->span, "A set cannot be indexed.",
                "Sets are unordered; use 'in' to test membership.");
    return typeError();
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be indexed.");
  return typeError();
}

TypePtr Sema::inferSlice(SliceExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  if (expr.start) {
    TypePtr startType = inferExpr(*expr.start);
    requireInteger(startType, expr.start->span, "The slice start");
  }
  if (expr.end) {
    TypePtr endType = inferExpr(*expr.end);
    requireInteger(endType, expr.end->span, "The slice end");
  }
  if (expr.step) {
    TypePtr stepType = inferExpr(*expr.step);
    requireInteger(stepType, expr.step->span, "The slice step");
  }

  TypePtr object = resolveType(objectType);
  // A slice is a view over the source storage; its length is only known at
  // run time, so the static type is the dynamic array form.
  if (object->kind == TypeKind::Array) return typeArray(object->elem);
  if (object->kind == TypeKind::String) return typeString();
  if (object->kind == TypeKind::Bytes) return typeBytes();
  if (object->kind == TypeKind::Tuple) {
    if (!object->params.empty()) {
      bool same = true;
      for (const auto& p : object->params)
        if (resolveType(p)->kind != resolveType(object->params[0])->kind) { same = false; break; }
      if (same) return typeArray(object->params[0]);
    }
    auto t = std::make_shared<Type>(); t->kind = TypeKind::Var; t->allowUnresolved = true; return typeArray(t);
  }
  if (object->kind == TypeKind::Var || object->kind == TypeKind::Union ||
      object->kind == TypeKind::Any) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be sliced.");
  return typeError();
}

TypePtr Sema::inferField(FieldExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Module) {
    ModuleAlias alias;
    alias.native = object->moduleId < 0;
    alias.moduleId = object->moduleId;
    alias.nativeName = object->structName;
    return moduleMemberType(alias, expr.field, expr.fieldSpan, true);
  }

  if (object->kind == TypeKind::Enum) {
    auto enumIt = enums_.find(object->structName);
    if (enumIt == enums_.end()) return typeError();
    EnumInfo& info = enumIt->second;
    if (info.builtinKind) {
      sink_.error(expr.fieldSpan,
                  "'" + shortName(object->structName) + "' is a prelude type; its values are "
                  "built-in.",
                  "Write Some(value), None, Ok(value), or Err(error) without a module prefix.");
      return typeError();
    }
    if (!info.algebraic) {
      if (info.values.count(expr.field)) return typeInt(32, true);
      sink_.error(expr.fieldSpan,
                  "Enum '" + shortName(object->structName) + "' has no variant named '" +
                      expr.field + "'.");
      return typeError();
    }
    if (enumNamespaceUse(expr.object, object->structName)) {
      const VariantInfo* variant = info.findVariant(expr.field);
      if (!variant) {
        sink_.error(expr.fieldSpan,
                    "Enum '" + shortName(object->structName) + "' has no variant named '" +
                        expr.field + "'.");
        return typeError();
      }
      if (!variant->fields.empty()) {
        sink_.error(expr.fieldSpan,
                    "Variant '" + shortName(info.name) + "." + variant->name +
                        "' carries a payload and must be constructed.",
                    "Call it: " + shortName(info.name) + "." + variant->name + "(...).");
        return typeError();
      }
      TypePtr t = typeEnum(info.name);
      for (std::size_t i = 0; i < info.typeParamVars.size(); ++i) {
        TypePtr fresh = freshVar(Constraint::None, expr.fieldSpan,
                                 "type parameter of '" + shortName(info.name) + "'");
        fresh->allowUnresolved = true;
        t->params.push_back(fresh);
      }
      return t;
    }
    // Payload access on a value: the field must belong to exactly one variant.
    const VariantInfo* owner = nullptr;
    const std::pair<std::string, TypePtr>* field = nullptr;
    for (const VariantInfo& v : info.variants) {
      for (const auto& f : v.fields) {
        if (f.first != expr.field) continue;
        if (owner) {
          sink_.error(expr.fieldSpan,
                      "Field '" + expr.field + "' exists on multiple variants of '" +
                          shortName(info.name) + "'.",
                      "Use `match` to select the variant first.");
          return typeError();
        }
        owner = &v;
        field = &f;
      }
    }
    if (!owner) {
      sink_.error(expr.fieldSpan,
                  "No variant of '" + shortName(info.name) + "' has a field named '" +
                      expr.field + "'.");
      return typeError();
    }
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < info.typeParamVars.size() && i < object->params.size(); ++i) {
      mapping[info.typeParamVars[i]] = object->params[i];
    }
    return instantiateType(field->second, mapping);
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < structIt->second.typeParamVars.size() && i < object->params.size(); ++i) {
      mapping[structIt->second.typeParamVars[i]] = object->params[i];
    }
    for (const auto& field : structIt->second.fields) {
      if (field.first == expr.field) {
        return instantiateType(field.second, mapping);
      }
    }
    sink_.error(expr.fieldSpan,
                "Struct '" + object->structName + "' has no field named '" + expr.field + "'.");
    return typeError();
  }

  if (object->kind == TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "Cannot access field '" + expr.field + "' on " + typePhrase(object) + ".",
                "Use '?.' to access fields of an optional value.");
    return typeError();
  }

  if (object->kind == TypeKind::Var || object->kind == TypeKind::Union ||
      object->kind == TypeKind::Any) {
    /* dynamic: inferred at runtime */ auto t=std::make_shared<Type>(); t->kind=TypeKind::Var; t->allowUnresolved=true; return t;
  }

  if (object->kind == TypeKind::Array || object->kind == TypeKind::String) {
    std::string help = expr.field == "len" ? "Use len(value) instead." : "";
    sink_.error(expr.fieldSpan,
                "Values of type '" + typeName(object) + "' have no fields.", help);
    return typeError();
  }

  sink_.error(expr.fieldSpan,
              "Values of type '" + typeName(object) + "' have no field '" + expr.field + "'.");
  return typeError();
}

TypePtr Sema::inferArrayLiteral(ArrayLitExpr& expr) {
  if (expr.elements.empty()) {
    TypePtr element = freshVar(Constraint::None, expr.span, "the element type of an empty array");
    element->allowUnresolved = true;
    return typeArray(element);
  }

  TypePtr elementType = inferExpr(*expr.elements[0]);
  for (std::size_t i = 1; i < expr.elements.size(); ++i) {
    TypePtr nextType = inferExpr(*expr.elements[i]);
    if (!unify(elementType, nextType)) {
      sink_.error(expr.elements[i]->span,
                  "All array elements must have the same type, found " + typeName(elementType) +
                      " and " + typeName(nextType) + ".");
      return typeError();
    }
  }
  return typeArray(elementType);
}

TypePtr Sema::inferMapLiteral(MapLitExpr& expr) {
  if (expr.entries.empty()) {
    TypePtr key = freshVar(Constraint::None, expr.span, "the key type of an empty map");
    TypePtr value = freshVar(Constraint::None, expr.span, "the value type of an empty map");
    key->allowUnresolved = true; value->allowUnresolved = true;
    return typeMap(key, value);
  }

  TypePtr keyType = inferExpr(*expr.entries[0].first);
  TypePtr valueType = inferExpr(*expr.entries[0].second);
  for (std::size_t i = 1; i < expr.entries.size(); ++i) {
    TypePtr nextKey = inferExpr(*expr.entries[i].first);
    TypePtr nextValue = inferExpr(*expr.entries[i].second);
    if (!unify(keyType, nextKey)) {
      sink_.error(expr.entries[i].first->span,
                  "All map keys must have the same type, found " + typeName(keyType) + " and " +
                      typeName(nextKey) + ".");
      keyType = typeError();
    }
    if (!unify(valueType, nextValue)) {
      sink_.error(expr.entries[i].second->span,
                  "All map values must have the same type, found " + typeName(valueType) +
                      " and " + typeName(nextValue) + ".");
      valueType = typeError();
    }
  }
  return typeMap(keyType, valueType);
}

TypePtr Sema::inferSetLiteral(SetLitExpr& expr) {
  if (expr.elements.empty()) {
    TypePtr element = freshVar(Constraint::None, expr.span, "the element type of an empty set");
    element->allowUnresolved = true;
    return typeSet(element);
  }

  TypePtr elementType = inferExpr(*expr.elements[0]);
  for (std::size_t i = 1; i < expr.elements.size(); ++i) {
    TypePtr nextType = inferExpr(*expr.elements[i]);
    if (!unify(elementType, nextType)) {
      sink_.error(expr.elements[i]->span,
                  "All set elements must have the same type, found " + typeName(elementType) +
                      " and " + typeName(nextType) + ".");
      return typeError();
    }
  }
  return typeSet(elementType);
}

TypePtr Sema::inferTuple(TupleExpr& expr) {
  std::vector<TypePtr> elements;
  elements.reserve(expr.elements.size());
  for (auto& element : expr.elements) {
    TypePtr type = inferExpr(*element);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(element->span, "A tuple element cannot be a void value.");
      type = typeError();
    }
    elements.push_back(type);
  }
  return typeTuple(std::move(elements));
}

TypePtr Sema::inferLambda(LambdaExpr& expr) {
  std::vector<TypePtr> params;
  params.reserve(expr.params.size());
  for (auto& param : expr.params) {
    TypePtr type;
    if (param.type) {
      type = resolveTypeExpr(*param.type);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(param.span, "Lambda parameter '" + param.name + "' cannot have type void.");
        type = typeError();
      }
    } else {
      type = freshVar(Constraint::None, param.span,
                      "the type of lambda parameter '" + param.name + "'");
    }
    params.push_back(type);
  }

  TypePtr ret = expr.returnType
                    ? resolveTypeExpr(*expr.returnType)
                    : freshVar(Constraint::None, expr.span, "the return type of a lambda");

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    declareVar(expr.params[i].name, expr.params[i].span, params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, ret)) {
    reportMismatch(bodyType, ret, expr.body->span, "in the lambda result");
  }
  popScope();

  return typeFunction(std::move(params), std::move(ret));
}

void Sema::checkLambdaAgainst(LambdaExpr& expr, TypePtr expected) {
  TypePtr fn = resolveType(expected);
  if (fn->kind != TypeKind::Function) {
    inferLambda(expr);
    return;
  }
  if (expr.params.size() != fn->params.size()) {
    sink_.error(expr.span,
                "This lambda declares " + std::to_string(expr.params.size()) +
                    " parameter(s), but the expected function type takes " +
                    std::to_string(fn->params.size()) + ".");
    inferLambda(expr);
    return;
  }

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    if (expr.params[i].type) {
      TypePtr declared = resolveTypeExpr(*expr.params[i].type);
      if (!unify(declared, fn->params[i])) {
        reportMismatch(declared, fn->params[i], expr.params[i].span,
                       "in parameter '" + expr.params[i].name + "' of the lambda");
      }
    }
    declareVar(expr.params[i].name, expr.params[i].span, fn->params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, fn->ret)) {
    reportMismatch(bodyType, fn->ret, expr.body->span, "in the lambda result");
  }
  popScope();
}

void Sema::checkArgAgainst(Expr& expr, TypePtr expected, const std::string& context) {
  if (auto* lambda = dynamic_cast<LambdaExpr*>(&expr)) {
    checkLambdaAgainst(*lambda, expected);
    return;
  }
  TypePtr actual = inferExpr(expr);
  if (!coerceVectorInit(expr, expected) && !unify(actual, expected)) {
    reportMismatch(actual, expected, expr.span, context);
    return;
  }
  checkFixedArrayLength(expr, expected);
}

// Vector coercion for call arguments / annotated bindings: a list literal
// of exactly N elements or a numeric scalar (broadcast) is accepted where
// vector[T, N] is expected. On success expr.inferredType becomes the vector
// type so llvm_emit produces unboxed <N x ty> values.
bool Sema::coerceVectorInit(Expr& expr, TypePtr expected) {
  TypePtr exp = resolveType(expected);
  if (!exp || exp->kind != TypeKind::Vector) return false;
  TypePtr actual = resolveType(expr.inferredType);
  if (!actual || actual->kind == TypeKind::Vector ||
      actual->kind == TypeKind::Error) {
    return false;
  }
  if (auto* lit = dynamic_cast<ArrayLitExpr*>(&expr)) {
    if (actual->kind != TypeKind::Array ||
        (std::int64_t)lit->elements.size() != exp->length) {
      return false;
    }
    if (!unify(actual->elem, exp->elem)) return false;
  } else {
    if (actual->kind != TypeKind::Var && !isNumericType(actual)) return false;
    if (!unify(actual, exp->elem)) return false;
  }
  expr.inferredType = exp;
  exprTypes_[&expr] = exp;
  return true;
}

// A fixed-size array type with a literal initializer must match exactly. The
// check recurses so `i32[2][3]` also validates the nested rows.
void Sema::checkFixedArrayLength(Expr& value, TypePtr expected) {
  TypePtr resolved = resolveType(expected);
  if (!resolved || resolved->kind != TypeKind::Array || resolved->length < 0) return;
  auto* literal = dynamic_cast<ArrayLitExpr*>(&value);
  if (!literal) return;
  const std::int64_t actual = static_cast<std::int64_t>(literal->elements.size());
  if (actual != resolved->length) {
    sink_.error(literal->span,
                "Fixed-size array " + typeName(resolved) + " requires " +
                    std::to_string(resolved->length) + " element(s), found " +
                    std::to_string(actual) + ".");
    return;
  }
  for (auto& element : literal->elements) checkFixedArrayLength(*element, resolved->elem);
}

TypePtr Sema::inferSafeNav(SafeNavExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Error) return typeError();
  if (object->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ (void)0;
    return typeError();
  }
  if (object->kind != TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "The left side of '?.' must be optional, found " + typePhrase(object) + ".",
                "Use '.' to access fields of a non-optional value.");
    return typeError();
  }
  TypePtr inner = resolveType(object->elem);
  if (inner->kind == TypeKind::Var) {
    /* dynamic: inferred at runtime */ (void)0;
    return typeError();
  }
  if (inner->kind != TypeKind::Struct) {
    sink_.error(expr.fieldSpan,
                "'?.' can only access struct fields, but the optional holds " +
                    typePhrase(inner) + ".");
    return typeError();
  }
  auto structIt = structs_.find(inner->structName);
  if (structIt == structs_.end()) return typeError();
  for (const auto& field : structIt->second.fields) {
    if (field.first != expr.field) continue;
    bool isOptional = resolveType(field.second)->kind == TypeKind::Optional;
    expr.fieldOptional = isOptional;
    if (isOptional) return field.second;
    return typeOptional(field.second);
  }
  sink_.error(expr.fieldSpan,
              "Struct '" + inner->structName + "' has no field named '" + expr.field + "'.");
  return typeError();
}

}  // namespace a
