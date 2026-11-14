#include "pipeline/comptime.hpp"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"

namespace a {
namespace {

// ================================================================ AST clone

TypeExprPtr cloneTypeExpr(const TypeExpr& e);
ExprPtr cloneExpr(const Expr& e);
StmtPtr cloneStmt(const Stmt& s);

TypeExprPtr cloneTypeExpr(const TypeExpr& e) {
  if (auto* n = dynamic_cast<const NamedTypeExpr*>(&e)) {
    auto out = std::make_unique<NamedTypeExpr>();
    out->span = n->span;
    out->qualifiers = n->qualifiers;
    out->name = n->name;
    for (const auto& ta : n->typeArgs) out->typeArgs.push_back(cloneTypeExpr(*ta));
    return out;
  }
  if (auto* n = dynamic_cast<const ArrayTypeExpr*>(&e)) {
    auto out = std::make_unique<ArrayTypeExpr>();
    out->span = n->span;
    out->element = cloneTypeExpr(*n->element);
    out->length = n->length;
    return out;
  }
  if (auto* n = dynamic_cast<const OptionalTypeExpr*>(&e)) {
    auto out = std::make_unique<OptionalTypeExpr>();
    out->span = n->span;
    out->element = cloneTypeExpr(*n->element);
    return out;
  }
  if (auto* n = dynamic_cast<const TupleTypeExpr*>(&e)) {
    auto out = std::make_unique<TupleTypeExpr>();
    out->span = n->span;
    for (const auto& el : n->elements) out->elements.push_back(cloneTypeExpr(*el));
    return out;
  }
  if (auto* n = dynamic_cast<const MapTypeExpr*>(&e)) {
    auto out = std::make_unique<MapTypeExpr>();
    out->span = n->span;
    out->key = cloneTypeExpr(*n->key);
    out->value = cloneTypeExpr(*n->value);
    return out;
  }
  if (auto* n = dynamic_cast<const SetTypeExpr*>(&e)) {
    auto out = std::make_unique<SetTypeExpr>();
    out->span = n->span;
    out->element = cloneTypeExpr(*n->element);
    return out;
  }
  if (auto* n = dynamic_cast<const FunctionTypeExpr*>(&e)) {
    auto out = std::make_unique<FunctionTypeExpr>();
    out->span = n->span;
    for (const auto& p : n->params) out->params.push_back(cloneTypeExpr(*p));
    if (n->returnType) out->returnType = cloneTypeExpr(*n->returnType);
    return out;
  }
  if (auto* n = dynamic_cast<const UnionTypeExpr*>(&e)) {
    auto out = std::make_unique<UnionTypeExpr>();
    out->span = n->span;
    for (const auto& el : n->elements) out->elements.push_back(cloneTypeExpr(*el));
    return out;
  }
  if (auto* n = dynamic_cast<const EllipsisTypeExpr*>(&e)) {
    auto out = std::make_unique<EllipsisTypeExpr>();
    out->span = n->span;
    return out;
  }
  if (auto* n = dynamic_cast<const ConstTypeExpr*>(&e)) {
    auto out = std::make_unique<ConstTypeExpr>();
    out->span = n->span;
    out->value = n->value;
    return out;
  }
  return nullptr;
}

ParamDecl cloneParamDecl(const ParamDecl& p) {
  ParamDecl out;
  out.name = p.name;
  out.span = p.span;
  if (p.type) out.type = cloneTypeExpr(*p.type);
  if (p.defaultValue) out.defaultValue = cloneExpr(*p.defaultValue);
  out.variadic = p.variadic;
  return out;
}

Pattern clonePattern(const Pattern& p) {
  Pattern out;
  out.kind = p.kind;
  out.span = p.span;
  if (p.value) out.value = cloneExpr(*p.value);
  if (p.rangeStart) out.rangeStart = cloneExpr(*p.rangeStart);
  if (p.rangeEnd) out.rangeEnd = cloneExpr(*p.rangeEnd);
  if (p.ctor) out.ctor = cloneExpr(*p.ctor);
  for (const auto& sub : p.subpatterns) out.subpatterns.push_back(clonePattern(sub));
  out.variantFields = p.variantFields;
  out.name = p.name;
  return out;
}

std::vector<StmtPtr> cloneStmtVec(const std::vector<StmtPtr>& v) {
  std::vector<StmtPtr> out;
  out.reserve(v.size());
  for (const auto& s : v) out.push_back(cloneStmt(*s));
  return out;
}

std::vector<ExprPtr> cloneExprVec(const std::vector<ExprPtr>& v) {
  std::vector<ExprPtr> out;
  out.reserve(v.size());
  for (const auto& e : v) out.push_back(cloneExpr(*e));
  return out;
}

ExprPtr cloneExpr(const Expr& e) {
  switch (e.kind) {
    case ExprKind::IntLit: {
      auto out = std::make_unique<IntLitExpr>();
      out->span = e.span;
      out->value = static_cast<const IntLitExpr&>(e).value;
      return out;
    }
    case ExprKind::FloatLit: {
      auto out = std::make_unique<FloatLitExpr>();
      out->span = e.span;
      out->value = static_cast<const FloatLitExpr&>(e).value;
      return out;
    }
    case ExprKind::BoolLit: {
      auto out = std::make_unique<BoolLitExpr>();
      out->span = e.span;
      out->value = static_cast<const BoolLitExpr&>(e).value;
      return out;
    }
    case ExprKind::CharLit: {
      auto out = std::make_unique<CharLitExpr>();
      out->span = e.span;
      out->value = static_cast<const CharLitExpr&>(e).value;
      return out;
    }
    case ExprKind::StringLit: {
      auto out = std::make_unique<StringLitExpr>();
      out->span = e.span;
      const auto& src = static_cast<const StringLitExpr&>(e);
      for (const StringPart& part : src.parts) {
        StringPart np;
        np.isExpr = part.isExpr;
        np.text = part.text;
        if (part.expr) np.expr = cloneExpr(*part.expr);
        out->parts.push_back(std::move(np));
      }
      return out;
    }
    case ExprKind::Ident: {
      auto out = std::make_unique<IdentExpr>();
      out->span = e.span;
      out->name = static_cast<const IdentExpr&>(e).name;
      return out;
    }
    case ExprKind::Unary: {
      const auto& src = static_cast<const UnaryExpr&>(e);
      auto out = std::make_unique<UnaryExpr>();
      out->span = src.span;
      out->op = src.op;
      out->postfix = src.postfix;
      out->opSpan = src.opSpan;
      out->operand = cloneExpr(*src.operand);
      return out;
    }
    case ExprKind::Binary: {
      const auto& src = static_cast<const BinaryExpr&>(e);
      auto out = std::make_unique<BinaryExpr>();
      out->span = src.span;
      out->op = src.op;
      out->binop = src.binop;
      out->opSpan = src.opSpan;
      out->lhs = cloneExpr(*src.lhs);
      out->rhs = cloneExpr(*src.rhs);
      return out;
    }
    case ExprKind::Call: {
      const auto& src = static_cast<const CallExpr&>(e);
      auto out = std::make_unique<CallExpr>();
      out->span = src.span;
      out->callee = cloneExpr(*src.callee);
      for (const CallArg& arg : src.args) {
        CallArg na;
        na.name = arg.name;
        na.nameSpan = arg.nameSpan;
        na.value = cloneExpr(*arg.value);
        out->args.push_back(std::move(na));
      }
      out->calleeResolved = src.calleeResolved;
      out->calleeFunction = src.calleeFunction;
      out->calleeStruct = src.calleeStruct;
      out->builtinId = src.builtinId;
      out->calleeIsBuiltin = src.calleeIsBuiltin;
      out->calleeBuiltin = src.calleeBuiltin;
      for (const auto& ta : src.typeArgs) {
        out->typeArgs.push_back(cloneTypeExpr(*ta));
      }
      return out;
    }
    case ExprKind::Index: {
      const auto& src = static_cast<const IndexExpr&>(e);
      auto out = std::make_unique<IndexExpr>();
      out->span = src.span;
      out->object = cloneExpr(*src.object);
      out->index = cloneExpr(*src.index);
      out->bracketSpan = src.bracketSpan;
      return out;
    }
    case ExprKind::Slice: {
      const auto& src = static_cast<const SliceExpr&>(e);
      auto out = std::make_unique<SliceExpr>();
      out->span = src.span;
      out->object = cloneExpr(*src.object);
      if (src.start) out->start = cloneExpr(*src.start);
      if (src.end) out->end = cloneExpr(*src.end);
      if (src.step) out->step = cloneExpr(*src.step);
      out->bracketSpan = src.bracketSpan;
      return out;
    }
    case ExprKind::Field: {
      const auto& src = static_cast<const FieldExpr&>(e);
      auto out = std::make_unique<FieldExpr>();
      out->span = src.span;
      out->object = cloneExpr(*src.object);
      out->field = src.field;
      out->fieldSpan = src.fieldSpan;
      return out;
    }
    case ExprKind::SafeNav: {
      const auto& src = static_cast<const SafeNavExpr&>(e);
      auto out = std::make_unique<SafeNavExpr>();
      out->span = src.span;
      out->object = cloneExpr(*src.object);
      out->field = src.field;
      out->fieldSpan = src.fieldSpan;
      out->fieldOptional = src.fieldOptional;
      return out;
    }
    case ExprKind::ArrayLit: {
      const auto& src = static_cast<const ArrayLitExpr&>(e);
      auto out = std::make_unique<ArrayLitExpr>();
      out->span = src.span;
      out->elements = cloneExprVec(src.elements);
      return out;
    }
    case ExprKind::Range: {
      const auto& src = static_cast<const RangeExpr&>(e);
      auto out = std::make_unique<RangeExpr>();
      out->span = src.span;
      out->start = cloneExpr(*src.start);
      out->end = cloneExpr(*src.end);
      if (src.step) out->step = cloneExpr(*src.step);
      return out;
    }
    case ExprKind::Ternary: {
      const auto& src = static_cast<const TernaryExpr&>(e);
      auto out = std::make_unique<TernaryExpr>();
      out->span = src.span;
      out->cond = cloneExpr(*src.cond);
      out->thenValue = cloneExpr(*src.thenValue);
      out->elseValue = cloneExpr(*src.elseValue);
      return out;
    }
    case ExprKind::Tuple: {
      const auto& src = static_cast<const TupleExpr&>(e);
      auto out = std::make_unique<TupleExpr>();
      out->span = src.span;
      out->elements = cloneExprVec(src.elements);
      return out;
    }
    case ExprKind::Lambda: {
      const auto& src = static_cast<const LambdaExpr&>(e);
      auto out = std::make_unique<LambdaExpr>();
      out->span = src.span;
      for (const ParamDecl& p : src.params) out->params.push_back(cloneParamDecl(p));
      if (src.returnType) out->returnType = cloneTypeExpr(*src.returnType);
      if (src.body) out->body = cloneExpr(*src.body);
      return out;
    }
    case ExprKind::Comprehension: {
      const auto& src = static_cast<const ComprehensionExpr&>(e);
      auto out = std::make_unique<ComprehensionExpr>();
      out->span = src.span;
      out->element = cloneExpr(*src.element);
      out->var = src.var;
      out->varSpan = src.varSpan;
      out->iterable = cloneExpr(*src.iterable);
      if (src.condition) out->condition = cloneExpr(*src.condition);
      out->isSet = src.isSet;
      return out;
    }
    case ExprKind::Match: {
      const auto& src = static_cast<const MatchExpr&>(e);
      auto out = std::make_unique<MatchExpr>();
      out->span = src.span;
      out->subject = cloneExpr(*src.subject);
      for (const MatchArm& arm : src.arms) {
        MatchArm na;
        na.pattern = clonePattern(arm.pattern);
        if (arm.guard) na.guard = cloneExpr(*arm.guard);
        na.result = cloneExpr(*arm.result);
        out->arms.push_back(std::move(na));
      }
      return out;
    }
    case ExprKind::Try: {
      const auto& src = static_cast<const TryExpr&>(e);
      auto out = std::make_unique<TryExpr>();
      out->span = src.span;
      out->operand = cloneExpr(*src.operand);
      return out;
    }
    case ExprKind::MapLit: {
      const auto& src = static_cast<const MapLitExpr&>(e);
      auto out = std::make_unique<MapLitExpr>();
      out->span = src.span;
      out->bracketSpan = src.bracketSpan;
      for (const auto& entry : src.entries) {
        out->entries.emplace_back(cloneExpr(*entry.first), cloneExpr(*entry.second));
      }
      return out;
    }
    case ExprKind::SetLit: {
      const auto& src = static_cast<const SetLitExpr&>(e);
      auto out = std::make_unique<SetLitExpr>();
      out->span = src.span;
      out->bracketSpan = src.bracketSpan;
      out->elements = cloneExprVec(src.elements);
      return out;
    }
  }
  return nullptr;
}

FieldDecl cloneFieldDecl(const FieldDecl& f) {
  FieldDecl out;
  out.name = f.name;
  out.span = f.span;
  if (f.type) out.type = cloneTypeExpr(*f.type);
  out.doc = f.doc;
  return out;
}

EnumVariant cloneEnumVariant(const EnumVariant& v) {
  EnumVariant out;
  out.name = v.name;
  out.span = v.span;
  if (v.value) out.value = cloneExpr(*v.value);
  for (const FieldDecl& f : v.fields) out.fields.push_back(cloneFieldDecl(f));
  out.doc = v.doc;
  return out;
}

TraitSig cloneTraitSig(const TraitSig& s) {
  TraitSig out;
  out.name = s.name;
  out.span = s.span;
  for (const ParamDecl& p : s.params) out.params.push_back(cloneParamDecl(p));
  if (s.returnType) out.returnType = cloneTypeExpr(*s.returnType);
  out.doc = s.doc;
  return out;
}

std::unique_ptr<FnDecl> cloneFnDecl(const FnDecl& d) {
  auto out = std::make_unique<FnDecl>();
  out->span = d.span;
  out->kind = d.kind;
  out->name = d.name;
  out->nameSpan = d.nameSpan;
  out->typeParams = d.typeParams;
  for (const ComptimeParam& p : d.comptimeParams) {
    ComptimeParam cp;
    cp.name = p.name;
    cp.span = p.span;
    if (p.type) cp.type = cloneTypeExpr(*p.type);
    cp.position = p.position;
    out->comptimeParams.push_back(std::move(cp));
  }
  for (const ParamDecl& p : d.params) out->params.push_back(cloneParamDecl(p));
  if (d.returnType) out->returnType = cloneTypeExpr(*d.returnType);
  out->whereClause = d.whereClause;
  out->whereSpans = d.whereSpans;
  out->whereSpan = d.whereSpan;
  out->body = cloneStmtVec(d.body);
  out->moduleId = d.moduleId;
  out->isEntry = d.isEntry;
  out->doc = d.doc;
  return out;
}

StmtPtr cloneStmt(const Stmt& s) {
  switch (s.kind) {
    case StmtKind::Let: {
      const auto& src = static_cast<const LetStmt&>(s);
      auto out = std::make_unique<LetStmt>();
      out->span = src.span;
      out->name = src.name;
      out->nameSpan = src.nameSpan;
      if (src.type) out->type = cloneTypeExpr(*src.type);
      out->mutableBinding = src.mutableBinding;
      if (src.init) out->init = cloneExpr(*src.init);
      return out;
    }
    case StmtKind::Assign: {
      const auto& src = static_cast<const AssignStmt&>(s);
      auto out = std::make_unique<AssignStmt>();
      out->span = src.span;
      out->op = src.op;
      out->binop = src.binop;
      out->opSpan = src.opSpan;
      out->target = cloneExpr(*src.target);
      out->value = cloneExpr(*src.value);
      return out;
    }
    case StmtKind::Expr: {
      const auto& src = static_cast<const ExprStmt&>(s);
      auto out = std::make_unique<ExprStmt>();
      out->span = src.span;
      out->expr = cloneExpr(*src.expr);
      return out;
    }
    case StmtKind::Destructure: {
      const auto& src = static_cast<const DestructureStmt&>(s);
      auto out = std::make_unique<DestructureStmt>();
      out->span = src.span;
      out->names = src.names;
      out->nameSpans = src.nameSpans;
      out->mutableBinding = src.mutableBinding;
      out->value = cloneExpr(*src.value);
      return out;
    }
    case StmtKind::Return: {
      const auto& src = static_cast<const ReturnStmt&>(s);
      auto out = std::make_unique<ReturnStmt>();
      out->span = src.span;
      if (src.value) out->value = cloneExpr(*src.value);
      return out;
    }
    case StmtKind::Break: {
      auto out = std::make_unique<BreakStmt>();
      out->span = s.span;
      return out;
    }
    case StmtKind::Continue: {
      auto out = std::make_unique<ContinueStmt>();
      out->span = s.span;
      return out;
    }
    case StmtKind::Defer: {
      const auto& src = static_cast<const DeferStmt&>(s);
      auto out = std::make_unique<DeferStmt>();
      out->span = src.span;
      out->body = cloneStmt(*src.body);
      return out;
    }
    case StmtKind::Import: {
      const auto& src = static_cast<const ImportStmt&>(s);
      auto out = std::make_unique<ImportStmt>();
      out->span = src.span;
      out->path = src.path;
      out->alias = src.alias;
      out->bareName = src.bareName;
      out->pythonModule = src.pythonModule;
      out->stdModule = src.stdModule;
      out->quoted = src.quoted;
      out->nativeModule = src.nativeModule;
      out->pythonModuleImport = src.pythonModuleImport;
      out->stdModuleImport = src.stdModuleImport;
      out->moduleId = src.moduleId;
      out->targetSpan = src.targetSpan;
      return out;
    }
    case StmtKind::FromImport: {
      const auto& src = static_cast<const FromImportStmt&>(s);
      auto out = std::make_unique<FromImportStmt>();
      out->span = src.span;
      out->path = src.path;
      out->bareName = src.bareName;
      out->pythonModule = src.pythonModule;
      out->stdModule = src.stdModule;
      out->quoted = src.quoted;
      out->nativeModule = src.nativeModule;
      out->pythonModuleImport = src.pythonModuleImport;
      out->stdModuleImport = src.stdModuleImport;
      out->moduleId = src.moduleId;
      out->targetSpan = src.targetSpan;
      for (const auto& item : src.items) {
        FromImportStmt::Item ni;
        ni.name = item.name;
        ni.alias = item.alias;
        ni.span = item.span;
        out->items.push_back(std::move(ni));
      }
      return out;
    }
    case StmtKind::If: {
      const auto& src = static_cast<const IfStmt&>(s);
      auto out = std::make_unique<IfStmt>();
      out->span = src.span;
      for (const auto& branch : src.branches) {
        IfStmt::Branch nb;
        nb.cond = cloneExpr(*branch.cond);
        nb.body = cloneStmtVec(branch.body);
        out->branches.push_back(std::move(nb));
      }
      out->elseBody = cloneStmtVec(src.elseBody);
      out->comptime = src.comptime;
      return out;
    }
    case StmtKind::While: {
      const auto& src = static_cast<const WhileStmt&>(s);
      auto out = std::make_unique<WhileStmt>();
      out->span = src.span;
      out->cond = cloneExpr(*src.cond);
      out->body = cloneStmtVec(src.body);
      return out;
    }
    case StmtKind::DoWhile: {
      const auto& src = static_cast<const DoWhileStmt&>(s);
      auto out = std::make_unique<DoWhileStmt>();
      out->span = src.span;
      out->body = cloneStmtVec(src.body);
      out->cond = cloneExpr(*src.cond);
      return out;
    }
    case StmtKind::For: {
      const auto& src = static_cast<const ForStmt&>(s);
      auto out = std::make_unique<ForStmt>();
      out->span = src.span;
      out->var = src.var;
      out->varSpan = src.varSpan;
      out->iterable = cloneExpr(*src.iterable);
      out->body = cloneStmtVec(src.body);
      out->comptime = src.comptime;
      return out;
    }
    case StmtKind::Loop: {
      const auto& src = static_cast<const LoopStmt&>(s);
      auto out = std::make_unique<LoopStmt>();
      out->span = src.span;
      out->body = cloneStmtVec(src.body);
      return out;
    }
    case StmtKind::FnDecl:
      return cloneFnDecl(static_cast<const FnDecl&>(s));
    case StmtKind::StructDecl: {
      const auto& src = static_cast<const StructDecl&>(s);
      auto out = std::make_unique<StructDecl>();
      out->span = src.span;
      out->kind = src.kind;
      out->name = src.name;
      out->nameSpan = src.nameSpan;
      out->typeParams = src.typeParams;
      out->baseClass = src.baseClass;
      out->metaclass = src.metaclass;
      for (const FieldDecl& f : src.fields) out->fields.push_back(cloneFieldDecl(f));
      for (const auto& m : src.methods) out->methods.push_back(cloneFnDecl(*m));
      out->doc = src.doc;
      return out;
    }
    case StmtKind::EnumDecl: {
      const auto& src = static_cast<const EnumDecl&>(s);
      auto out = std::make_unique<EnumDecl>();
      out->span = src.span;
      out->kind = src.kind;
      out->name = src.name;
      out->nameSpan = src.nameSpan;
      out->typeParams = src.typeParams;
      for (const EnumVariant& v : src.variants) out->variants.push_back(cloneEnumVariant(v));
      out->doc = src.doc;
      return out;
    }
    case StmtKind::TypeAlias: {
      const auto& src = static_cast<const TypeAliasDecl&>(s);
      auto out = std::make_unique<TypeAliasDecl>();
      out->span = src.span;
      out->kind = src.kind;
      out->name = src.name;
      out->nameSpan = src.nameSpan;
      if (src.type) out->type = cloneTypeExpr(*src.type);
      return out;
    }
    case StmtKind::TraitDecl: {
      const auto& src = static_cast<const TraitDecl&>(s);
      auto out = std::make_unique<TraitDecl>();
      out->span = src.span;
      out->kind = src.kind;
      out->name = src.name;
      out->nameSpan = src.nameSpan;
      out->typeParams = src.typeParams;
      for (const TraitSig& sig : src.methods) out->methods.push_back(cloneTraitSig(sig));
      out->parents = src.parents;
      out->whereClause = src.whereClause;
      out->whereSpans = src.whereSpans;
      out->doc = src.doc;
      return out;
    }
    case StmtKind::ImplDecl: {
      const auto& src = static_cast<const ImplDecl&>(s);
      auto out = std::make_unique<ImplDecl>();
      out->span = src.span;
      out->kind = src.kind;
      out->traitName = src.traitName;
      out->traitSpan = src.traitSpan;
      out->targetName = src.targetName;
      out->targetSpan = src.targetSpan;
      for (const auto& m : src.methods) out->methods.push_back(cloneFnDecl(*m));
      return out;
    }
  }
  return nullptr;
}

// ============================================= comptime value + evaluator

struct CVal {
  enum Kind { Int, Float, Bool, Str, NoneV, List, Tuple } kind = NoneV;
  long long i = 0;
  double f = 0.0;
  bool b = false;
  std::string s;
  std::vector<CVal> items;

  static CVal integer(long long v) { CVal c; c.kind = Int; c.i = v; return c; }
  static CVal flt(double v) { CVal c; c.kind = Float; c.f = v; return c; }
  static CVal boolean(bool v) { CVal c; c.kind = Bool; c.b = v; return c; }
  static CVal str(std::string v) { CVal c; c.kind = Str; c.s = std::move(v); return c; }
  static CVal null() { return CVal{}; }
  static CVal list(std::vector<CVal> v) {
    CVal c; c.kind = List; c.items = std::move(v); return c;
  }
  static CVal tup(std::vector<CVal> v) {
    CVal c; c.kind = Tuple; c.items = std::move(v); return c;
  }
};

bool cvalTruthy(const CVal& v) {
  switch (v.kind) {
    case CVal::Int: return v.i != 0;
    case CVal::Float: return v.f != 0.0;
    case CVal::Bool: return v.b;
    case CVal::Str: return !v.s.empty();
    case CVal::NoneV: return false;
    case CVal::List:
    case CVal::Tuple: return !v.items.empty();
  }
  return false;
}

bool cvalNumeric(const CVal& v) { return v.kind == CVal::Int || v.kind == CVal::Float; }

double cvalToDouble(const CVal& v) { return v.kind == CVal::Int ? static_cast<double>(v.i) : v.f; }

std::string cvalToString(const CVal& v) {
  switch (v.kind) {
    case CVal::Int: return std::to_string(v.i);
    case CVal::Float: {
      std::string s = std::to_string(v.f);
      while (s.size() > 1 && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      return s;
    }
    case CVal::Bool: return v.b ? "True" : "False";
    case CVal::Str: return v.s;
    case CVal::NoneV: return "None";
    case CVal::List:
    case CVal::Tuple: return "<" + std::string(v.kind == CVal::List ? "list" : "tuple") + ">";
  }
  return {};
}

bool cvalEqual(const CVal& a, const CVal& b) {
  if (a.kind != b.kind) {
    if (cvalNumeric(a) && cvalNumeric(b)) return cvalToDouble(a) == cvalToDouble(b);
    return false;
  }
  switch (a.kind) {
    case CVal::Int: return a.i == b.i;
    case CVal::Float: return a.f == b.f;
    case CVal::Bool: return a.b == b.b;
    case CVal::Str: return a.s == b.s;
    case CVal::NoneV: return true;
    case CVal::List:
    case CVal::Tuple:
      if (a.items.size() != b.items.size()) return false;
      for (std::size_t i = 0; i < a.items.size(); ++i)
        if (!cvalEqual(a.items[i], b.items[i])) return false;
      return true;
  }
  return false;
}

// Converts a comptime value back into a literal AST expression so it can be
// substituted into a specialized body.
ExprPtr cvalToExpr(const CVal& v, Span span) {
  switch (v.kind) {
    case CVal::Int: {
      auto e = std::make_unique<IntLitExpr>();
      e->span = span;
      e->value = v.i;
      return e;
    }
    case CVal::Float: {
      auto e = std::make_unique<FloatLitExpr>();
      e->span = span;
      e->value = v.f;
      return e;
    }
    case CVal::Bool: {
      auto e = std::make_unique<BoolLitExpr>();
      e->span = span;
      e->value = v.b;
      return e;
    }
    case CVal::Str: {
      auto e = std::make_unique<StringLitExpr>();
      e->span = span;
      StringPart part;
      part.isExpr = false;
      part.text = v.s;
      e->parts.push_back(std::move(part));
      return e;
    }
    case CVal::NoneV: {
      auto e = std::make_unique<IdentExpr>();
      e->span = span;
      e->name = "None";
      return e;
    }
    case CVal::List: {
      auto e = std::make_unique<ArrayLitExpr>();
      e->span = span;
      for (const CVal& item : v.items) e->elements.push_back(cvalToExpr(item, span));
      return e;
    }
    case CVal::Tuple: {
      auto e = std::make_unique<TupleExpr>();
      e->span = span;
      for (const CVal& item : v.items) e->elements.push_back(cvalToExpr(item, span));
      return e;
    }
  }
  return nullptr;
}

std::optional<CVal> evalComptime(const Expr& e);

std::optional<CVal> evalCallComptime(const CallExpr& call) {
  const IdentExpr* id = dynamic_cast<const IdentExpr*>(call.callee.get());
  if (!id) return std::nullopt;
  const std::string& name = id->name;
  auto argVals = [&]() -> std::optional<std::vector<CVal>> {
    std::vector<CVal> out;
    for (const CallArg& arg : call.args) {
      if (!arg.name.empty()) return std::nullopt;
      auto v = evalComptime(*arg.value);
      if (!v) return std::nullopt;
      out.push_back(std::move(*v));
    }
    return out;
  };
  if (name == "range") {
    auto args = argVals();
    if (!args) return std::nullopt;
    long long start = 0, stop = 0, step = 1;
    if (args->size() == 1) {
      if ((*args)[0].kind != CVal::Int) return std::nullopt;
      stop = (*args)[0].i;
    } else if (args->size() == 2) {
      if ((*args)[0].kind != CVal::Int || (*args)[1].kind != CVal::Int) return std::nullopt;
      start = (*args)[0].i;
      stop = (*args)[1].i;
    } else if (args->size() == 3) {
      if ((*args)[0].kind != CVal::Int || (*args)[1].kind != CVal::Int ||
          (*args)[2].kind != CVal::Int)
        return std::nullopt;
      start = (*args)[0].i;
      stop = (*args)[1].i;
      step = (*args)[2].i;
    } else {
      return std::nullopt;
    }
    if (step == 0) return std::nullopt;
    std::vector<CVal> items;
    if (step > 0) {
      for (long long v = start; v < stop && items.size() < 100000; v += step)
        items.push_back(CVal::integer(v));
    } else {
      for (long long v = start; v > stop && items.size() < 100000; v += step)
        items.push_back(CVal::integer(v));
    }
    return CVal::list(std::move(items));
  }
  if (name == "len") {
    auto args = argVals();
    if (!args || args->size() != 1) return std::nullopt;
    const CVal& v = (*args)[0];
    if (v.kind == CVal::Str) return CVal::integer(static_cast<long long>(v.s.size()));
    if (v.kind == CVal::List || v.kind == CVal::Tuple)
      return CVal::integer(static_cast<long long>(v.items.size()));
    return std::nullopt;
  }
  if (name == "str" || name == "int" || name == "float" || name == "bool") {
    auto args = argVals();
    if (!args || args->size() != 1) return std::nullopt;
    const CVal& v = (*args)[0];
    if (name == "str") return CVal::str(cvalToString(v));
    if (name == "int") {
      if (v.kind == CVal::Int) return v;
      if (v.kind == CVal::Float) return CVal::integer(static_cast<long long>(v.f));
      if (v.kind == CVal::Bool) return CVal::integer(v.b ? 1 : 0);
      if (v.kind == CVal::Str) {
        try {
          return CVal::integer(std::stoll(v.s));
        } catch (...) {
          return std::nullopt;
        }
      }
      return std::nullopt;
    }
    if (name == "float") {
      if (cvalNumeric(v)) return CVal::flt(cvalToDouble(v));
      if (v.kind == CVal::Bool) return CVal::flt(v.b ? 1.0 : 0.0);
      return std::nullopt;
    }
    return CVal::boolean(cvalTruthy(v));  // bool(...)
  }
  if (name == "abs") {
    auto args = argVals();
    if (!args || args->size() != 1) return std::nullopt;
    const CVal& v = (*args)[0];
    if (v.kind == CVal::Int) return CVal::integer(v.i < 0 ? -v.i : v.i);
    if (v.kind == CVal::Float) return CVal::flt(std::fabs(v.f));
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<CVal> evalComptime(const Expr& e) {
  switch (e.kind) {
    case ExprKind::IntLit:
      return CVal::integer(static_cast<const IntLitExpr&>(e).value);
    case ExprKind::FloatLit:
      return CVal::flt(static_cast<const FloatLitExpr&>(e).value);
    case ExprKind::BoolLit:
      return CVal::boolean(static_cast<const BoolLitExpr&>(e).value);
    case ExprKind::CharLit: {
      char32_t cp = static_cast<const CharLitExpr&>(e).value;
      std::string s;
      if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
      } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      return CVal::str(std::move(s));
    }
    case ExprKind::StringLit: {
      std::string out;
      for (const StringPart& part : static_cast<const StringLitExpr&>(e).parts) {
        if (part.isExpr) {
          auto v = evalComptime(*part.expr);
          if (!v) return std::nullopt;
          out += cvalToString(*v);
        } else {
          out += part.text;
        }
      }
      return CVal::str(std::move(out));
    }
    case ExprKind::Ident: {
      const std::string& name = static_cast<const IdentExpr&>(e).name;
      if (name == "None") return CVal::null();
      if (name == "True") return CVal::boolean(true);
      if (name == "False") return CVal::boolean(false);
      return std::nullopt;
    }
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(e);
      if (u.postfix) return std::nullopt;
      auto v = evalComptime(*u.operand);
      if (!v) return std::nullopt;
      if (u.op == "-") {
        if (v->kind == CVal::Int) return CVal::integer(-v->i);
        if (v->kind == CVal::Float) return CVal::flt(-v->f);
        return std::nullopt;
      }
      if (u.op == "~") {
        if (v->kind == CVal::Int) return CVal::integer(~v->i);
        return std::nullopt;
      }
      if (u.op == "not" || u.op == "!") return CVal::boolean(!cvalTruthy(*v));
      return std::nullopt;
    }
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(e);
      if (b.binop == BinOp::And) {
        auto l = evalComptime(*b.lhs);
        if (!l) return std::nullopt;
        if (!cvalTruthy(*l)) return CVal::boolean(false);
        auto r = evalComptime(*b.rhs);
        if (!r) return std::nullopt;
        return CVal::boolean(cvalTruthy(*r));
      }
      if (b.binop == BinOp::Or) {
        auto l = evalComptime(*b.lhs);
        if (!l) return std::nullopt;
        if (cvalTruthy(*l)) return CVal::boolean(true);
        auto r = evalComptime(*b.rhs);
        if (!r) return std::nullopt;
        return CVal::boolean(cvalTruthy(*r));
      }
      auto l = evalComptime(*b.lhs);
      if (!l) return std::nullopt;
      auto r = evalComptime(*b.rhs);
      if (!r) return std::nullopt;
      const CVal& a = *l;
      const CVal& c = *r;
      switch (b.binop) {
        case BinOp::Add:
          if (a.kind == CVal::Str && c.kind == CVal::Str) return CVal::str(a.s + c.s);
          if (a.kind == CVal::List && c.kind == CVal::List) {
            std::vector<CVal> items = a.items;
            items.insert(items.end(), c.items.begin(), c.items.end());
            return CVal::list(std::move(items));
          }
          if (a.kind == CVal::Tuple && c.kind == CVal::Tuple) {
            std::vector<CVal> items = a.items;
            items.insert(items.end(), c.items.begin(), c.items.end());
            return CVal::tup(std::move(items));
          }
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i + c.i);
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::flt(cvalToDouble(a) + cvalToDouble(c));
          return std::nullopt;
        case BinOp::Sub:
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i - c.i);
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::flt(cvalToDouble(a) - cvalToDouble(c));
          return std::nullopt;
        case BinOp::Mul:
          if (a.kind == CVal::Str && c.kind == CVal::Int) {
            std::string out;
            for (long long k = 0; k < c.i && k < 1000000; ++k) out += a.s;
            return CVal::str(std::move(out));
          }
          if (a.kind == CVal::Int && c.kind == CVal::Str) {
            std::string out;
            for (long long k = 0; k < a.i && k < 1000000; ++k) out += c.s;
            return CVal::str(std::move(out));
          }
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i * c.i);
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::flt(cvalToDouble(a) * cvalToDouble(c));
          return std::nullopt;
        case BinOp::Div:
          if (cvalNumeric(a) && cvalNumeric(c) && cvalToDouble(c) != 0.0)
            return CVal::flt(cvalToDouble(a) / cvalToDouble(c));
          return std::nullopt;
        case BinOp::FloorDiv:
          if (a.kind == CVal::Int && c.kind == CVal::Int && c.i != 0)
            return CVal::integer(a.i / c.i);
          if (cvalNumeric(a) && cvalNumeric(c) && cvalToDouble(c) != 0.0)
            return CVal::flt(std::floor(cvalToDouble(a) / cvalToDouble(c)));
          return std::nullopt;
        case BinOp::Mod:
          if (a.kind == CVal::Int && c.kind == CVal::Int && c.i != 0)
            return CVal::integer(a.i % c.i);
          if (cvalNumeric(a) && cvalNumeric(c) && cvalToDouble(c) != 0.0)
            return CVal::flt(std::fmod(cvalToDouble(a), cvalToDouble(c)));
          return std::nullopt;
        case BinOp::Pow: {
          if (a.kind == CVal::Int && c.kind == CVal::Int && c.i >= 0 && c.i < 64) {
            long long result = 1;
            for (long long k = 0; k < c.i; ++k) result *= a.i;
            return CVal::integer(result);
          }
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::flt(std::pow(cvalToDouble(a), cvalToDouble(c)));
          return std::nullopt;
        }
        case BinOp::Eq: return CVal::boolean(cvalEqual(a, c));
        case BinOp::Ne: return CVal::boolean(!cvalEqual(a, c));
        case BinOp::Lt:
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::boolean(cvalToDouble(a) < cvalToDouble(c));
          if (a.kind == CVal::Str && c.kind == CVal::Str) return CVal::boolean(a.s < c.s);
          return std::nullopt;
        case BinOp::Gt:
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::boolean(cvalToDouble(a) > cvalToDouble(c));
          if (a.kind == CVal::Str && c.kind == CVal::Str) return CVal::boolean(a.s > c.s);
          return std::nullopt;
        case BinOp::Le:
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::boolean(cvalToDouble(a) <= cvalToDouble(c));
          if (a.kind == CVal::Str && c.kind == CVal::Str) return CVal::boolean(a.s <= c.s);
          return std::nullopt;
        case BinOp::Ge:
          if (cvalNumeric(a) && cvalNumeric(c))
            return CVal::boolean(cvalToDouble(a) >= cvalToDouble(c));
          if (a.kind == CVal::Str && c.kind == CVal::Str) return CVal::boolean(a.s >= c.s);
          return std::nullopt;
        case BinOp::BitAnd:
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i & c.i);
          return std::nullopt;
        case BinOp::BitOr:
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i | c.i);
          return std::nullopt;
        case BinOp::BitXor:
          if (a.kind == CVal::Int && c.kind == CVal::Int) return CVal::integer(a.i ^ c.i);
          return std::nullopt;
        case BinOp::Shl:
          if (a.kind == CVal::Int && c.kind == CVal::Int && c.i >= 0 && c.i < 63)
            return CVal::integer(a.i << c.i);
          return std::nullopt;
        case BinOp::Shr:
          if (a.kind == CVal::Int && c.kind == CVal::Int && c.i >= 0 && c.i < 63)
            return CVal::integer(a.i >> c.i);
          return std::nullopt;
        case BinOp::In:
        case BinOp::NotIn: {
          bool found = false;
          if (c.kind == CVal::Str && a.kind == CVal::Str)
            found = c.s.find(a.s) != std::string::npos;
          else if (c.kind == CVal::List || c.kind == CVal::Tuple) {
            for (const CVal& item : c.items)
              if (cvalEqual(a, item)) {
                found = true;
                break;
              }
          } else {
            return std::nullopt;
          }
          return CVal::boolean(b.binop == BinOp::In ? found : !found);
        }
        case BinOp::Coalesce: {
          if (a.kind != CVal::NoneV) return a;
          return c;
        }
        default:
          return std::nullopt;
      }
    }
    case ExprKind::Ternary: {
      const auto& t = static_cast<const TernaryExpr&>(e);
      auto cond = evalComptime(*t.cond);
      if (!cond) return std::nullopt;
      return evalComptime(cvalTruthy(*cond) ? *t.thenValue : *t.elseValue);
    }
    case ExprKind::ArrayLit: {
      std::vector<CVal> items;
      for (const auto& el : static_cast<const ArrayLitExpr&>(e).elements) {
        auto v = evalComptime(*el);
        if (!v) return std::nullopt;
        items.push_back(std::move(*v));
      }
      return CVal::list(std::move(items));
    }
    case ExprKind::Tuple: {
      std::vector<CVal> items;
      for (const auto& el : static_cast<const TupleExpr&>(e).elements) {
        auto v = evalComptime(*el);
        if (!v) return std::nullopt;
        items.push_back(std::move(*v));
      }
      return CVal::tup(std::move(items));
    }
    case ExprKind::Range: {
      const auto& r = static_cast<const RangeExpr&>(e);
      auto start = evalComptime(*r.start);
      auto end = evalComptime(*r.end);
      if (!start || !end || start->kind != CVal::Int || end->kind != CVal::Int)
        return std::nullopt;
      long long step = 1;
      if (r.step) {
        auto st = evalComptime(*r.step);
        if (!st || st->kind != CVal::Int) return std::nullopt;
        step = st->i;
      }
      if (step == 0) return std::nullopt;
      std::vector<CVal> items;
      if (step > 0) {
        for (long long v = start->i; v < end->i && items.size() < 100000; v += step)
          items.push_back(CVal::integer(v));
      } else {
        for (long long v = start->i; v > end->i && items.size() < 100000; v += step)
          items.push_back(CVal::integer(v));
      }
      return CVal::list(std::move(items));
    }
    case ExprKind::Index: {
      const auto& ix = static_cast<const IndexExpr&>(e);
      auto obj = evalComptime(*ix.object);
      auto idx = evalComptime(*ix.index);
      if (!obj || !idx || idx->kind != CVal::Int) return std::nullopt;
      if ((obj->kind == CVal::List || obj->kind == CVal::Tuple) && idx->i >= 0 &&
          static_cast<std::size_t>(idx->i) < obj->items.size()) {
        return obj->items[static_cast<std::size_t>(idx->i)];
      }
      if (obj->kind == CVal::Str && idx->i >= 0 &&
          static_cast<std::size_t>(idx->i) < obj->s.size()) {
        return CVal::str(std::string(1, obj->s[static_cast<std::size_t>(idx->i)]));
      }
      return std::nullopt;
    }
    case ExprKind::Call:
      return evalCallComptime(static_cast<const CallExpr&>(e));
    default:
      return std::nullopt;
  }
}

// ==================================================== comptime substitution

// Replaces IdentExpr references to comptime names with clones of the bound
// literal expressions. `shadow` holds names that locally rebind one of those
// names (lambda params, loop vars, lets, ...); those occurrences are left
// alone so `let N = ...` inside a body still refers to the local.
using SubstBindings = std::unordered_map<std::string, const Expr*>;
using ShadowSet = std::unordered_set<std::string>;

void substExprPtr(ExprPtr& e, const SubstBindings& bindings, const ShadowSet& shadow);
void substStmtInPlace(Stmt& s, const SubstBindings& bindings, ShadowSet& shadow);
void substStmtVecInPlace(std::vector<StmtPtr>& stmts, const SubstBindings& bindings,
                         ShadowSet& shadow);
void substFnDecl(FnDecl& fn, const SubstBindings& bindings);

ExprPtr substExprClone(const Expr& e, const SubstBindings& bindings,
                       const ShadowSet& shadow) {
  ExprPtr copy = cloneExpr(e);
  substExprPtr(copy, bindings, shadow);
  return copy;
}

void collectPatternBindings(const Pattern& p, ShadowSet& out) {
  if (p.kind == Pattern::Kind::Binding && !p.name.empty()) out.insert(p.name);
  for (const auto& sub : p.subpatterns) collectPatternBindings(sub, out);
}

void substExprPtr(ExprPtr& e, const SubstBindings& bindings, const ShadowSet& shadow) {
  if (!e) return;
  if (e->kind == ExprKind::Ident) {
    const auto& id = static_cast<const IdentExpr&>(*e);
    auto it = bindings.find(id.name);
    if (it != bindings.end() && !shadow.count(id.name)) {
      e = substExprClone(*it->second, bindings, shadow);
      return;
    }
    return;
  }
  switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
    case ExprKind::Ident:
      return;
    case ExprKind::StringLit: {
      auto& s = static_cast<StringLitExpr&>(*e);
      for (auto& part : s.parts)
        if (part.expr) substExprPtr(part.expr, bindings, shadow);
      return;
    }
    case ExprKind::Unary: {
      substExprPtr(static_cast<UnaryExpr&>(*e).operand, bindings, shadow);
      return;
    }
    case ExprKind::Binary: {
      auto& b = static_cast<BinaryExpr&>(*e);
      substExprPtr(b.lhs, bindings, shadow);
      substExprPtr(b.rhs, bindings, shadow);
      return;
    }
    case ExprKind::Call: {
      auto& c = static_cast<CallExpr&>(*e);
      substExprPtr(c.callee, bindings, shadow);
      for (auto& arg : c.args) substExprPtr(arg.value, bindings, shadow);
      return;
    }
    case ExprKind::Index: {
      auto& x = static_cast<IndexExpr&>(*e);
      substExprPtr(x.object, bindings, shadow);
      substExprPtr(x.index, bindings, shadow);
      return;
    }
    case ExprKind::Slice: {
      auto& x = static_cast<SliceExpr&>(*e);
      substExprPtr(x.object, bindings, shadow);
      if (x.start) substExprPtr(x.start, bindings, shadow);
      if (x.end) substExprPtr(x.end, bindings, shadow);
      if (x.step) substExprPtr(x.step, bindings, shadow);
      return;
    }
    case ExprKind::Field: {
      substExprPtr(static_cast<FieldExpr&>(*e).object, bindings, shadow);
      return;
    }
    case ExprKind::SafeNav: {
      substExprPtr(static_cast<SafeNavExpr&>(*e).object, bindings, shadow);
      return;
    }
    case ExprKind::ArrayLit: {
      auto& a = static_cast<ArrayLitExpr&>(*e);
      for (auto& el : a.elements) substExprPtr(el, bindings, shadow);
      return;
    }
    case ExprKind::Range: {
      auto& r = static_cast<RangeExpr&>(*e);
      substExprPtr(r.start, bindings, shadow);
      substExprPtr(r.end, bindings, shadow);
      if (r.step) substExprPtr(r.step, bindings, shadow);
      return;
    }
    case ExprKind::Ternary: {
      auto& t = static_cast<TernaryExpr&>(*e);
      substExprPtr(t.cond, bindings, shadow);
      substExprPtr(t.thenValue, bindings, shadow);
      substExprPtr(t.elseValue, bindings, shadow);
      return;
    }
    case ExprKind::Tuple: {
      auto& t = static_cast<TupleExpr&>(*e);
      for (auto& el : t.elements) substExprPtr(el, bindings, shadow);
      return;
    }
    case ExprKind::Lambda: {
      auto& l = static_cast<LambdaExpr&>(*e);
      for (auto& p : l.params)
        if (p.defaultValue) substExprPtr(p.defaultValue, bindings, shadow);
      ShadowSet inner = shadow;
      for (const ParamDecl& p : l.params) inner.insert(p.name);
      substExprPtr(l.body, bindings, inner);
      return;
    }
    case ExprKind::Comprehension: {
      auto& c = static_cast<ComprehensionExpr&>(*e);
      substExprPtr(c.iterable, bindings, shadow);
      ShadowSet inner = shadow;
      inner.insert(c.var);
      substExprPtr(c.element, bindings, inner);
      if (c.condition) substExprPtr(c.condition, bindings, inner);
      return;
    }
    case ExprKind::Match: {
      auto& m = static_cast<MatchExpr&>(*e);
      substExprPtr(m.subject, bindings, shadow);
      for (auto& arm : m.arms) {
        if (arm.pattern.value) substExprPtr(arm.pattern.value, bindings, shadow);
        if (arm.pattern.rangeStart) substExprPtr(arm.pattern.rangeStart, bindings, shadow);
        if (arm.pattern.rangeEnd) substExprPtr(arm.pattern.rangeEnd, bindings, shadow);
        if (arm.pattern.ctor) substExprPtr(arm.pattern.ctor, bindings, shadow);
        for (auto& sub : arm.pattern.subpatterns) {
          // Pattern sub-expressions are walked shallowly; nested patterns
          // with value exprs are rare and handled one level deep here.
          if (sub.value) substExprPtr(sub.value, bindings, shadow);
        }
        ShadowSet inner = shadow;
        collectPatternBindings(arm.pattern, inner);
        if (arm.guard) substExprPtr(arm.guard, bindings, inner);
        substExprPtr(arm.result, bindings, inner);
      }
      return;
    }
    case ExprKind::Try: {
      substExprPtr(static_cast<TryExpr&>(*e).operand, bindings, shadow);
      return;
    }
    case ExprKind::MapLit: {
      auto& m = static_cast<MapLitExpr&>(*e);
      for (auto& entry : m.entries) {
        substExprPtr(entry.first, bindings, shadow);
        substExprPtr(entry.second, bindings, shadow);
      }
      return;
    }
    case ExprKind::SetLit: {
      auto& s = static_cast<SetLitExpr&>(*e);
      for (auto& el : s.elements) substExprPtr(el, bindings, shadow);
      return;
    }
  }
}

void substStmtVecInPlace(std::vector<StmtPtr>& stmts, const SubstBindings& bindings,
                         ShadowSet& shadow) {
  for (auto& stmt : stmts) substStmtInPlace(*stmt, bindings, shadow);
}

void substFnDecl(FnDecl& fn, const SubstBindings& bindings) {
  ShadowSet shadow;
  for (const auto& p : fn.params) shadow.insert(p.name);
  for (auto& p : fn.params)
    if (p.defaultValue) substExprPtr(p.defaultValue, bindings, shadow);
  substStmtVecInPlace(fn.body, bindings, shadow);
}

void substStmtInPlace(Stmt& s, const SubstBindings& bindings, ShadowSet& shadow) {
  switch (s.kind) {
    case StmtKind::Let: {
      auto& l = static_cast<LetStmt&>(s);
      if (l.init) substExprPtr(l.init, bindings, shadow);
      shadow.insert(l.name);
      return;
    }
    case StmtKind::Assign: {
      auto& a = static_cast<AssignStmt&>(s);
      substExprPtr(a.value, bindings, shadow);
      // An identifier target that names a comptime binding becomes a fresh
      // local (assignment to a comptime parameter is not allowed); anything
      // else is substituted normally, including index/field targets.
      if (auto* id = dynamic_cast<IdentExpr*>(a.target.get())) {
        if (bindings.count(id->name)) {
          shadow.insert(id->name);
          return;
        }
      }
      substExprPtr(a.target, bindings, shadow);
      return;
    }
    case StmtKind::Expr: {
      substExprPtr(static_cast<ExprStmt&>(s).expr, bindings, shadow);
      return;
    }
    case StmtKind::Destructure: {
      auto& d = static_cast<DestructureStmt&>(s);
      substExprPtr(d.value, bindings, shadow);
      for (const auto& n : d.names)
        if (n != "_") shadow.insert(n);
      return;
    }
    case StmtKind::Return: {
      auto& r = static_cast<ReturnStmt&>(s);
      if (r.value) substExprPtr(r.value, bindings, shadow);
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      return;
    case StmtKind::Defer: {
      ShadowSet inner = shadow;
      substStmtInPlace(*static_cast<DeferStmt&>(s).body, bindings, inner);
      return;
    }
    case StmtKind::Import:
    case StmtKind::FromImport:
      return;
    case StmtKind::If: {
      auto& f = static_cast<IfStmt&>(s);
      for (auto& branch : f.branches) {
        substExprPtr(branch.cond, bindings, shadow);
        ShadowSet inner = shadow;
        substStmtVecInPlace(branch.body, bindings, inner);
      }
      ShadowSet inner = shadow;
      substStmtVecInPlace(f.elseBody, bindings, inner);
      return;
    }
    case StmtKind::While: {
      auto& w = static_cast<WhileStmt&>(s);
      substExprPtr(w.cond, bindings, shadow);
      ShadowSet inner = shadow;
      substStmtVecInPlace(w.body, bindings, inner);
      return;
    }
    case StmtKind::DoWhile: {
      auto& d = static_cast<DoWhileStmt&>(s);
      ShadowSet inner = shadow;
      substStmtVecInPlace(d.body, bindings, inner);
      substExprPtr(d.cond, bindings, shadow);
      return;
    }
    case StmtKind::For: {
      auto& f = static_cast<ForStmt&>(s);
      substExprPtr(f.iterable, bindings, shadow);
      ShadowSet inner = shadow;
      inner.insert(f.var);
      substStmtVecInPlace(f.body, bindings, inner);
      return;
    }
    case StmtKind::Loop: {
      auto& l = static_cast<LoopStmt&>(s);
      ShadowSet inner = shadow;
      substStmtVecInPlace(l.body, bindings, inner);
      return;
    }
    case StmtKind::FnDecl: {
      substFnDecl(static_cast<FnDecl&>(s), bindings);
      return;
    }
    case StmtKind::StructDecl: {
      auto& st = static_cast<StructDecl&>(s);
      for (auto& m : st.methods) substFnDecl(*m, bindings);
      return;
    }
    case StmtKind::EnumDecl: {
      auto& en = static_cast<EnumDecl&>(s);
      for (auto& v : en.variants)
        if (v.value) substExprPtr(v.value, bindings, shadow);
      return;
    }
    case StmtKind::TypeAlias:
    case StmtKind::TraitDecl:
      return;
    case StmtKind::ImplDecl: {
      auto& im = static_cast<ImplDecl&>(s);
      for (auto& m : im.methods) substFnDecl(*m, bindings);
      return;
    }
  }
}

// ============================================================ module index

struct ModuleInfo {
  std::unordered_map<std::string, FnDecl*> fns;
  std::unordered_set<std::string> topValues;
  struct FromItem {
    std::string alias;
    std::string sourceName;
    int moduleId = -1;
  };
  std::vector<FromItem> fromItems;
  std::unordered_map<std::string, int> importAliases;
};

std::string importAliasOf(const ImportStmt& imp) {
  if (!imp.alias.empty()) return imp.alias;
  if (imp.quoted || imp.stdModuleImport) {
    std::string name = imp.quoted ? imp.path : imp.stdModule;
    std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
  }
  if (imp.pythonModuleImport) {
    std::string name = imp.pythonModule;
    std::size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(dot + 1);
    return name;
  }
  return imp.bareName;
}

// Finds the FnDecl named `name` visible as a member of module `moduleId`,
// following `from X import name` re-exports. `moduleOut` receives the id of
// the module that actually declares the function.
FnDecl* findFnDeclInModule(const std::vector<ModuleInfo>& infos, int moduleId,
                           const std::string& name, int* moduleOut, int depth = 0) {
  if (moduleId < 0 || depth > 8 || moduleId >= static_cast<int>(infos.size()))
    return nullptr;
  const ModuleInfo& info = infos[static_cast<std::size_t>(moduleId)];
  auto it = info.fns.find(name);
  if (it != info.fns.end()) {
    if (moduleOut) *moduleOut = moduleId;
    return it->second;
  }
  for (const ModuleInfo::FromItem& item : info.fromItems) {
    if (item.alias != name || item.moduleId < 0) continue;
    return findFnDeclInModule(infos, item.moduleId, item.sourceName, moduleOut, depth + 1);
  }
  return nullptr;
}

// ============================================================ the pass

struct Spec {
  FnDecl* decl = nullptr;  // non-owning; owned by the module's statement list
  std::string name;
  int moduleId = -1;
};

struct Pass {
  ModuleGraph& graph;
  DiagnosticSink& sink;
  std::vector<ModuleInfo> infos;
  std::vector<Spec> specs;
  std::unordered_map<std::string, std::size_t> specByKey;
  std::set<std::pair<int, std::size_t>> syntheticImports;
  std::vector<std::vector<StmtPtr>> pendingTopLevel;
  std::unordered_set<const CallExpr*> reportedCalls;
  int specCounter = 0;
  bool changed = false;
  // True while walking a function that still has comptime parameters (the
  // original, unsubstituted declaration). Comptime arguments there may
  // reference the parameters themselves and are not compile-time constants;
  // such calls are left alone silently because the declaration is only ever
  // reached through specializations.
  bool inComptimeDecl = false;

  static constexpr std::size_t kMaxSpecs = 512;
  static constexpr std::size_t kMaxUnroll = 4096;
  static constexpr int kMaxRounds = 32;

  Pass(ModuleGraph& g, DiagnosticSink& s) : graph(g), sink(s) {
    infos.resize(graph.modules.size());
    pendingTopLevel.resize(graph.modules.size());
    for (const Module& mod : graph.modules) {
      if (!mod.program) continue;
      ModuleInfo& info = infos[static_cast<std::size_t>(mod.id)];
      for (const StmtPtr& stmt : mod.program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
          if (!info.fns.count(fn->name)) info.fns[fn->name] = fn;
        } else if (auto* let = dynamic_cast<LetStmt*>(stmt.get())) {
          info.topValues.insert(let->name);
        } else if (auto* asg = dynamic_cast<AssignStmt*>(stmt.get())) {
          if (asg->op == "=") {
            if (auto* id = dynamic_cast<IdentExpr*>(asg->target.get()))
              info.topValues.insert(id->name);
          }
        } else if (auto* dst = dynamic_cast<DestructureStmt*>(stmt.get())) {
          for (const auto& n : dst->names)
            if (n != "_") info.topValues.insert(n);
        } else if (auto* from = dynamic_cast<FromImportStmt*>(stmt.get())) {
          if (from->pythonModuleImport || from->nativeModule || from->moduleId < 0)
            continue;
          for (const auto& item : from->items) {
            info.fromItems.push_back(ModuleInfo::FromItem{
                item.alias.empty() ? item.name : item.alias, item.name, from->moduleId});
          }
        } else if (auto* imp = dynamic_cast<ImportStmt*>(stmt.get())) {
          if (imp->pythonModuleImport || imp->nativeModule || imp->moduleId < 0) continue;
          std::string alias = importAliasOf(*imp);
          if (!alias.empty()) info.importAliases[alias] = imp->moduleId;
        }
      }
    }
  }

  void walkExpr(ExprPtr& e, int moduleId, ShadowSet& bound);
  void walkPatternExprs(Pattern& p, int moduleId, ShadowSet& bound);
  void walkStmts(std::vector<StmtPtr>& stmts, int moduleId, ShadowSet& bound);
  // Processes stmts[begin, end). Returns the index one past the last
  // processed statement; comptime splices inside the range adjust it.
  std::size_t walkStmtsRange(std::vector<StmtPtr>& stmts, std::size_t begin,
                             std::size_t end, int moduleId, ShadowSet& bound);
  void walkStmt(Stmt& s, int moduleId, ShadowSet& bound);
  void walkFn(FnDecl& fn, int moduleId);

  struct Resolved {
    FnDecl* decl = nullptr;
    int declModule = -1;
    bool viaModuleField = false;
  };

  Resolved resolveCallee(const CallExpr& call, int moduleId, const ShadowSet& bound);
  std::string cvalKey(const CVal& v) const;
  void trySpecialize(CallExpr& call, int moduleId, const ShadowSet& bound);
};

Pass::Resolved Pass::resolveCallee(const CallExpr& call, int moduleId,
                                   const ShadowSet& bound) {
  Resolved out;
  const ModuleInfo& info = infos[static_cast<std::size_t>(moduleId)];

  if (auto* id = dynamic_cast<const IdentExpr*>(call.callee.get())) {
    const std::string& name = id->name;
    if (bound.count(name)) return out;

    // from-imports take precedence over module-level functions (mirrors
    // Sema::inferCall: lookupVar -> fromBindings -> functionsByName_).
    for (const ModuleInfo::FromItem& item : info.fromItems) {
      if (item.alias != name || item.moduleId < 0) continue;
      int declModule = -1;
      FnDecl* found =
          findFnDeclInModule(infos, item.moduleId, item.sourceName, &declModule);
      if (found) {
        out.decl = found;
        out.declModule = declModule;
      }
      return out;  // first matching alias wins; non-fn bindings yield null
    }

    auto it = info.fns.find(name);
    if (it != info.fns.end()) {
      out.decl = it->second;
      out.declModule = moduleId;
    }
    return out;
  }

  if (auto* field = dynamic_cast<const FieldExpr*>(call.callee.get())) {
    auto* obj = dynamic_cast<const IdentExpr*>(field->object.get());
    if (!obj || bound.count(obj->name)) return out;
    auto aliasIt = info.importAliases.find(obj->name);
    if (aliasIt == info.importAliases.end()) return out;
    int declModule = -1;
    FnDecl* found =
        findFnDeclInModule(infos, aliasIt->second, field->field, &declModule);
    if (!found) return out;
    out.decl = found;
    out.declModule = declModule;
    out.viaModuleField = true;
    return out;
  }

  return out;
}

std::string Pass::cvalKey(const CVal& v) const {
  switch (v.kind) {
    case CVal::Int: return "i" + std::to_string(v.i);
    case CVal::Float: return "f" + std::to_string(v.f);
    case CVal::Bool: return v.b ? "b1" : "b0";
    case CVal::Str: return "s" + std::to_string(v.s.size()) + ":" + v.s;
    case CVal::NoneV: return "n";
    case CVal::List:
    case CVal::Tuple: {
      std::string out = v.kind == CVal::List ? "l[" : "t[";
      for (const CVal& item : v.items) out += cvalKey(item) + ",";
      return out + "]";
    }
  }
  return "?";
}

void Pass::trySpecialize(CallExpr& call, int moduleId, const ShadowSet& bound) {
  Resolved res = resolveCallee(call, moduleId, bound);
  FnDecl* decl = res.decl;
  if (!decl || decl->comptimeParams.empty() || decl->isEntry || res.declModule < 0)
    return;
  if (specs.size() >= kMaxSpecs) {
    if (reportedCalls.insert(&call).second) {
      sink.error(call.span, "Too many comptime specializations of '" + decl->name + "'.",
                 "Comptime parameters must produce a bounded set of specializations.");
    }
    return;
  }

  // v1: all arguments must be positional.
  for (const CallArg& arg : call.args) {
    if (!arg.name.empty()) return;
  }

  // Every comptime position must have a supplied argument.
  for (const ComptimeParam& cp : decl->comptimeParams) {
    if (cp.position >= call.args.size()) {
      if (reportedCalls.insert(&call).second) {
        sink.error(cp.span, "Missing comptime argument for '" + cp.name + "'.",
                   "Pass a compile-time constant for every comptime parameter.");
      }
      return;
    }
  }

  std::vector<CVal> values;
  values.reserve(decl->comptimeParams.size());
  std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(decl)) + "|";
  for (const ComptimeParam& cp : decl->comptimeParams) {
    const Expr& argExpr = *call.args[cp.position].value;
    auto v = evalComptime(argExpr);
    if (!v) {
      if (!inComptimeDecl && reportedCalls.insert(&call).second) {
        sink.error(argExpr.span,
                   "Comptime argument for '" + cp.name + "' of '" + decl->name +
                       "' must be a compile-time constant.",
                   "Use a literal or constant expression (e.g. 4, 2 + 2, len([1, 2])).");
      }
      return;
    }
    if (cp.type) {
      if (auto* named = dynamic_cast<NamedTypeExpr*>(cp.type.get())) {
        const std::string& tn = named->name;
        bool typeOk = true;
        if (tn == "int") typeOk = v->kind == CVal::Int || v->kind == CVal::Bool;
        else if (tn == "float") typeOk = cvalNumeric(*v);
        else if (tn == "bool") typeOk = v->kind == CVal::Bool;
        else if (tn == "str" || tn == "string") typeOk = v->kind == CVal::Str;
        if (!typeOk) {
          if (reportedCalls.insert(&call).second) {
            sink.error(argExpr.span, "Comptime argument for '" + cp.name +
                                           "' must be of type '" + tn + "'.");
          }
          return;
        }
      }
    }
    values.push_back(std::move(*v));
    key += cvalKey(values.back()) + ";";
  }

  std::size_t specIndex;
  auto existing = specByKey.find(key);
  if (existing != specByKey.end()) {
    specIndex = existing->second;
  } else {
    specIndex = specs.size();
    specByKey[key] = specIndex;

    std::string specName = decl->name + "__ct" + std::to_string(specCounter++);
    {
      // Unique within the declaring module.
      const ModuleInfo& dinfo = infos[static_cast<std::size_t>(res.declModule)];
      auto nameTaken = [&](const std::string& n) {
        if (dinfo.fns.count(n)) return true;
        for (const Spec& s : specs)
          if (s.moduleId == res.declModule && s.name == n) return true;
        return false;
      };
      while (nameTaken(specName)) specName = decl->name + "__ct" + std::to_string(specCounter++);
    }

    auto clone = cloneFnDecl(*decl);
    clone->name = specName;
    clone->comptimeParams.clear();

    SubstBindings bindings;
    std::vector<ExprPtr> keepAlive;
    for (std::size_t i = 0; i < decl->comptimeParams.size(); ++i) {
      ExprPtr lit = cvalToExpr(values[i], decl->comptimeParams[i].span);
      bindings[decl->comptimeParams[i].name] = lit.get();
      keepAlive.push_back(std::move(lit));
    }
    substFnDecl(*clone, bindings);

    Spec spec;
    spec.name = specName;
    spec.moduleId = res.declModule;
    spec.decl = clone.get();
    specs.push_back(spec);
    // The clone is owned by the declaring module's statement list from here
    // on, so Sema registers it and the backends emit it like any function.
    pendingTopLevel[static_cast<std::size_t>(res.declModule)].push_back(
        StmtPtr(static_cast<Stmt*>(clone.release())));
  }

  const Spec& spec = specs[specIndex];
  std::vector<CallArg> kept;
  kept.reserve(call.args.size());
  std::unordered_set<std::size_t> drop;
  for (const ComptimeParam& cp : decl->comptimeParams) drop.insert(cp.position);
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    if (!drop.count(i)) kept.push_back(std::move(call.args[i]));
  }
  call.args = std::move(kept);

  if (res.viaModuleField) {
    static_cast<FieldExpr&>(*call.callee).field = spec.name;
  } else {
    static_cast<IdentExpr&>(*call.callee).name = spec.name;
    if (res.declModule != moduleId) {
      auto key2 = std::make_pair(moduleId, specIndex);
      if (syntheticImports.insert(key2).second) {
        auto from = std::make_unique<FromImportStmt>();
        from->span = call.span;
        from->moduleId = res.declModule;
        FromImportStmt::Item item;
        item.name = spec.name;
        item.alias = spec.name;
        item.span = call.span;
        from->items.push_back(std::move(item));
        pendingTopLevel[static_cast<std::size_t>(moduleId)].push_back(std::move(from));
      }
    }
  }
  changed = true;
}

// ---------------------------------------------------------------- walkers

void Pass::walkPatternExprs(Pattern& p, int moduleId, ShadowSet& bound) {
  if (p.value) walkExpr(p.value, moduleId, bound);
  if (p.rangeStart) walkExpr(p.rangeStart, moduleId, bound);
  if (p.rangeEnd) walkExpr(p.rangeEnd, moduleId, bound);
  if (p.ctor) walkExpr(p.ctor, moduleId, bound);
  for (auto& sub : p.subpatterns) walkPatternExprs(sub, moduleId, bound);
}

void Pass::walkExpr(ExprPtr& e, int moduleId, ShadowSet& bound) {
  if (!e) return;
  if (e->kind == ExprKind::Call) {
    trySpecialize(static_cast<CallExpr&>(*e), moduleId, bound);
  }
  switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
    case ExprKind::Ident:
      return;
    case ExprKind::StringLit: {
      auto& s = static_cast<StringLitExpr&>(*e);
      for (auto& part : s.parts)
        if (part.expr) walkExpr(part.expr, moduleId, bound);
      return;
    }
    case ExprKind::Unary: {
      walkExpr(static_cast<UnaryExpr&>(*e).operand, moduleId, bound);
      return;
    }
    case ExprKind::Binary: {
      auto& b = static_cast<BinaryExpr&>(*e);
      walkExpr(b.lhs, moduleId, bound);
      walkExpr(b.rhs, moduleId, bound);
      return;
    }
    case ExprKind::Call: {
      auto& c = static_cast<CallExpr&>(*e);
      walkExpr(c.callee, moduleId, bound);
      for (auto& arg : c.args) walkExpr(arg.value, moduleId, bound);
      return;
    }
    case ExprKind::Index: {
      auto& x = static_cast<IndexExpr&>(*e);
      walkExpr(x.object, moduleId, bound);
      walkExpr(x.index, moduleId, bound);
      return;
    }
    case ExprKind::Slice: {
      auto& x = static_cast<SliceExpr&>(*e);
      walkExpr(x.object, moduleId, bound);
      if (x.start) walkExpr(x.start, moduleId, bound);
      if (x.end) walkExpr(x.end, moduleId, bound);
      if (x.step) walkExpr(x.step, moduleId, bound);
      return;
    }
    case ExprKind::Field: {
      walkExpr(static_cast<FieldExpr&>(*e).object, moduleId, bound);
      return;
    }
    case ExprKind::SafeNav: {
      walkExpr(static_cast<SafeNavExpr&>(*e).object, moduleId, bound);
      return;
    }
    case ExprKind::ArrayLit: {
      auto& a = static_cast<ArrayLitExpr&>(*e);
      for (auto& el : a.elements) walkExpr(el, moduleId, bound);
      return;
    }
    case ExprKind::Range: {
      auto& r = static_cast<RangeExpr&>(*e);
      walkExpr(r.start, moduleId, bound);
      walkExpr(r.end, moduleId, bound);
      if (r.step) walkExpr(r.step, moduleId, bound);
      return;
    }
    case ExprKind::Ternary: {
      auto& t = static_cast<TernaryExpr&>(*e);
      walkExpr(t.cond, moduleId, bound);
      walkExpr(t.thenValue, moduleId, bound);
      walkExpr(t.elseValue, moduleId, bound);
      return;
    }
    case ExprKind::Tuple: {
      auto& t = static_cast<TupleExpr&>(*e);
      for (auto& el : t.elements) walkExpr(el, moduleId, bound);
      return;
    }
    case ExprKind::Lambda: {
      auto& l = static_cast<LambdaExpr&>(*e);
      for (auto& p : l.params)
        if (p.defaultValue) walkExpr(p.defaultValue, moduleId, bound);
      ShadowSet inner = bound;
      for (const ParamDecl& p : l.params) inner.insert(p.name);
      walkExpr(l.body, moduleId, inner);
      return;
    }
    case ExprKind::Comprehension: {
      auto& c = static_cast<ComprehensionExpr&>(*e);
      walkExpr(c.iterable, moduleId, bound);
      ShadowSet inner = bound;
      inner.insert(c.var);
      walkExpr(c.element, moduleId, inner);
      if (c.condition) walkExpr(c.condition, moduleId, inner);
      return;
    }
    case ExprKind::Match: {
      auto& m = static_cast<MatchExpr&>(*e);
      walkExpr(m.subject, moduleId, bound);
      for (auto& arm : m.arms) {
        walkPatternExprs(arm.pattern, moduleId, bound);
        ShadowSet inner = bound;
        collectPatternBindings(arm.pattern, inner);
        if (arm.guard) walkExpr(arm.guard, moduleId, inner);
        walkExpr(arm.result, moduleId, inner);
      }
      return;
    }
    case ExprKind::Try: {
      walkExpr(static_cast<TryExpr&>(*e).operand, moduleId, bound);
      return;
    }
    case ExprKind::MapLit: {
      auto& m = static_cast<MapLitExpr&>(*e);
      for (auto& entry : m.entries) {
        walkExpr(entry.first, moduleId, bound);
        walkExpr(entry.second, moduleId, bound);
      }
      return;
    }
    case ExprKind::SetLit: {
      auto& s = static_cast<SetLitExpr&>(*e);
      for (auto& el : s.elements) walkExpr(el, moduleId, bound);
      return;
    }
  }
}

void Pass::walkStmt(Stmt& s, int moduleId, ShadowSet& bound) {
  switch (s.kind) {
    case StmtKind::Let: {
      auto& l = static_cast<LetStmt&>(s);
      if (l.init) walkExpr(l.init, moduleId, bound);
      bound.insert(l.name);
      return;
    }
    case StmtKind::Assign: {
      auto& a = static_cast<AssignStmt&>(s);
      walkExpr(a.value, moduleId, bound);
      if (a.op == "=") {
        if (auto* id = dynamic_cast<IdentExpr*>(a.target.get())) {
          bound.insert(id->name);
        } else {
          walkExpr(a.target, moduleId, bound);
        }
      } else {
        walkExpr(a.target, moduleId, bound);
      }
      return;
    }
    case StmtKind::Expr: {
      walkExpr(static_cast<ExprStmt&>(s).expr, moduleId, bound);
      return;
    }
    case StmtKind::Destructure: {
      auto& d = static_cast<DestructureStmt&>(s);
      walkExpr(d.value, moduleId, bound);
      for (const auto& n : d.names)
        if (n != "_") bound.insert(n);
      return;
    }
    case StmtKind::Return: {
      auto& r = static_cast<ReturnStmt&>(s);
      if (r.value) walkExpr(r.value, moduleId, bound);
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      return;
    case StmtKind::Defer: {
      ShadowSet inner = bound;
      walkStmt(*static_cast<DeferStmt&>(s).body, moduleId, inner);
      return;
    }
    case StmtKind::Import:
    case StmtKind::FromImport:
      return;
    case StmtKind::If: {
      auto& f = static_cast<IfStmt&>(s);
      // comptime if/for are resolved by walkStmts (which owns the parent
      // vector and can splice). Reaching here means the statement sits in a
      // single-statement context (defer); degrade to runtime control flow.
      for (auto& branch : f.branches) {
        walkExpr(branch.cond, moduleId, bound);
        ShadowSet inner = bound;
        walkStmts(branch.body, moduleId, inner);
      }
      {
        ShadowSet inner = bound;
        walkStmts(f.elseBody, moduleId, inner);
      }
      return;
    }
    case StmtKind::While: {
      auto& w = static_cast<WhileStmt&>(s);
      walkExpr(w.cond, moduleId, bound);
      ShadowSet inner = bound;
      walkStmts(w.body, moduleId, inner);
      return;
    }
    case StmtKind::DoWhile: {
      auto& d = static_cast<DoWhileStmt&>(s);
      ShadowSet inner = bound;
      walkStmts(d.body, moduleId, inner);
      walkExpr(d.cond, moduleId, bound);
      return;
    }
    case StmtKind::For: {
      auto& f = static_cast<ForStmt&>(s);
      walkExpr(f.iterable, moduleId, bound);
      ShadowSet inner = bound;
      inner.insert(f.var);
      walkStmts(f.body, moduleId, inner);
      return;
    }
    case StmtKind::Loop: {
      auto& l = static_cast<LoopStmt&>(s);
      ShadowSet inner = bound;
      walkStmts(l.body, moduleId, inner);
      return;
    }
    case StmtKind::FnDecl: {
      walkFn(static_cast<FnDecl&>(s), moduleId);
      return;
    }
    case StmtKind::StructDecl: {
      auto& st = static_cast<StructDecl&>(s);
      for (auto& m : st.methods) walkFn(*m, moduleId);
      return;
    }
    case StmtKind::EnumDecl: {
      auto& en = static_cast<EnumDecl&>(s);
      for (auto& v : en.variants)
        if (v.value) walkExpr(v.value, moduleId, bound);
      return;
    }
    case StmtKind::TypeAlias:
    case StmtKind::TraitDecl:
      return;
    case StmtKind::ImplDecl: {
      auto& im = static_cast<ImplDecl&>(s);
      for (auto& m : im.methods) walkFn(*m, moduleId);
      return;
    }
  }
}

void Pass::walkFn(FnDecl& fn, int moduleId) {
  ShadowSet bound;
  // Function bodies see every module-level value binding (Sema checks all
  // top-level statements before any function body).
  const ModuleInfo& info = infos[static_cast<std::size_t>(moduleId)];
  bound = info.topValues;
  for (const auto& p : fn.params) bound.insert(p.name);
  for (const auto& cp : fn.comptimeParams) bound.insert(cp.name);
  bool saved = inComptimeDecl;
  inComptimeDecl = !fn.comptimeParams.empty();
  walkStmts(fn.body, moduleId, bound);
  inComptimeDecl = saved;
}

std::size_t Pass::walkStmtsRange(std::vector<StmtPtr>& stmts, std::size_t begin,
                                 std::size_t end, int moduleId, ShadowSet& bound) {
  std::size_t i = begin;
  while (i < end && i < stmts.size()) {
    Stmt& s = *stmts[i];
    if (s.kind == StmtKind::If) {
      auto& f = static_cast<IfStmt&>(s);
      if (f.comptime) {
        // Evaluate the branch chain lazily.
        int taken = -1;
        bool allEval = true;
        for (std::size_t bi = 0; bi < f.branches.size(); ++bi) {
          auto c = evalComptime(*f.branches[bi].cond);
          if (!c) {
            allEval = false;
            break;
          }
          if (cvalTruthy(*c)) {
            taken = static_cast<int>(bi);
            break;
          }
        }
        if (allEval) {
          std::vector<StmtPtr> repl;
          if (taken >= 0) {
            repl = std::move(f.branches[static_cast<std::size_t>(taken)].body);
          } else {
            repl = std::move(f.elseBody);
          }
          stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
          stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i),
                       std::make_move_iterator(repl.begin()),
                       std::make_move_iterator(repl.end()));
          changed = true;
          // The inserted statements are walked with a scoped copy of bound
          // (branch binders do not leak into the enclosing block). Nested
          // comptime splices inside them adjust the region; fold the delta
          // back into `end`.
          ShadowSet inner = bound;
          std::size_t subEnd =
              walkStmtsRange(stmts, i, i + repl.size(), moduleId, inner);
          // Net change: -1 (the if) + final inserted count (subEnd - i).
          end = end + subEnd - i - 1;
          i = subEnd;
          continue;
        }
      }
      for (auto& branch : f.branches) {
        walkExpr(branch.cond, moduleId, bound);
        ShadowSet inner = bound;
        walkStmts(branch.body, moduleId, inner);
      }
      {
        ShadowSet inner = bound;
        walkStmts(f.elseBody, moduleId, inner);
      }
      ++i;
      continue;
    }
    if (s.kind == StmtKind::For) {
      auto& f = static_cast<ForStmt&>(s);
      if (f.comptime) {
        auto items = evalComptime(*f.iterable);
        if (items && (items->kind == CVal::List || items->kind == CVal::Tuple)) {
          if (items->items.empty()) {
            stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            --end;
            continue;
          }
          if (items->items.size() <= kMaxUnroll) {
            std::vector<StmtPtr> unrolled;
            for (const CVal& item : items->items) {
              ExprPtr lit = cvalToExpr(item, f.varSpan);
              SubstBindings bindings;
              bindings[f.var] = lit.get();
              std::vector<ExprPtr> keepAlive;
              keepAlive.push_back(std::move(lit));
              for (const StmtPtr& bodyStmt : f.body) {
                StmtPtr copy = cloneStmt(*bodyStmt);
                ShadowSet shadow;
                substStmtInPlace(*copy, bindings, shadow);
                unrolled.push_back(std::move(copy));
              }
            }
            stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
            stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i),
                         std::make_move_iterator(unrolled.begin()),
                         std::make_move_iterator(unrolled.end()));
            changed = true;
            ShadowSet inner = bound;
            std::size_t subEnd =
                walkStmtsRange(stmts, i, i + unrolled.size(), moduleId, inner);
            end = end + subEnd - i - 1;
            i = subEnd;
            continue;
          }
        }
      }
      walkExpr(f.iterable, moduleId, bound);
      {
        ShadowSet inner = bound;
        inner.insert(f.var);
        walkStmts(f.body, moduleId, inner);
      }
      ++i;
      continue;
    }
    walkStmt(s, moduleId, bound);
    ++i;
  }
  return end;
}

void Pass::walkStmts(std::vector<StmtPtr>& stmts, int moduleId, ShadowSet& bound) {
  (void)walkStmtsRange(stmts, 0, stmts.size(), moduleId, bound);
}

}  // namespace

void runComptimePass(ModuleGraph& graph, DiagnosticSink& sink) {
  Pass pass(graph, sink);
  // Fixpoint: specialization creates new decls whose bodies may contain more
  // comptime calls / comptime control flow; unrolling likewise.
  for (int round = 0; round < Pass::kMaxRounds; ++round) {
    pass.changed = false;
    for (std::size_t m = 0; m < graph.modules.size(); ++m) {
      Module& mod = graph.modules[m];
      if (!mod.program) continue;
      ShadowSet bound;
      pass.walkStmts(mod.program->statements, mod.id, bound);
      // Flush pending top-level statements (specialized decls, synthetic
      // imports) once this module's walk is done — appending earlier would
      // invalidate the vector being iterated.
      auto& pending = pass.pendingTopLevel[m];
      if (!pending.empty()) {
        for (auto& s : pending) mod.program->statements.push_back(std::move(s));
        pending.clear();
      }
    }
    if (!pass.changed) break;
  }
}

}  // namespace a
