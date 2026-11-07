#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
namespace {

// Snapshot of a value definition used by instCombine's def-use rules.
struct DefInfo {
  Op op = Op::Opaque;
  std::string text;
  TypePtr type;
  std::vector<int> operands;
  int intValue = 0;
  bool isConst = false;
  Const c;
};

using DefMap = std::unordered_map<int, DefInfo>;

const DefInfo* defOf(const DefMap& defs, int id) {
  if (id < 0) return nullptr;
  auto found = defs.find(id);
  return found == defs.end() ? nullptr : &found->second;
}

TypeKind kindOf(const DefInfo* def) {
  if (!def || !def->type) return TypeKind::Error;
  TypePtr resolved = resolveType(def->type);
  return resolved ? resolved->kind : TypeKind::Error;
}

bool isFloatKind(TypeKind kind) { return kind == TypeKind::Float; }

// Structural equality for the scalar types casts care about.
bool sameScalarType(const TypePtr& a, const TypePtr& b) {
  TypePtr x = resolveType(a);
  TypePtr y = resolveType(b);
  if (!x || !y || x->kind != y->kind) return false;
  if (x->kind == TypeKind::Int) return x->bits == y->bits && x->isSigned == y->isSigned;
  if (x->kind == TypeKind::Float) return x->bits == y->bits;
  return false;
}

bool isComparison(const std::string& op) {
  return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

bool isOrdering(const std::string& op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=";
}

// Combines a cast-of-a-cast chain. Returns the replacement cast kind and
// whether the inner cast can be bypassed (operand becomes the inner cast's
// operand). Only fires on rules that preserve LLVM cast semantics.
bool combineCastKind(const std::string& inner, const std::string& outer,
                     const TypePtr& innerDst, const TypePtr& outerDst,
                     const TypePtr& ultimateSrc, std::string& newKind) {
  if (inner == outer &&
      (inner == "trunc" || inner == "zext" || inner == "sext" || inner == "fpext" ||
       inner == "fptrunc" || inner == "bitcast")) {
    newKind = outer;
    return true;
  }
  // trunc(zext/sext x): a narrower trunc of an extended value is just a
  // trunc of the original; a wider "trunc" target is the original extension.
  if (outer == "trunc" && (inner == "zext" || inner == "sext")) {
    TypePtr s = resolveType(ultimateSrc);
    TypePtr d = resolveType(outerDst);
    TypePtr i = resolveType(innerDst);
    if (s && d && i && s->kind == TypeKind::Int && d->kind == TypeKind::Int &&
        i->kind == TypeKind::Int) {
      newKind = d->bits <= s->bits ? "trunc" : inner;
      return true;
    }
  }
  return false;
}

void snapshotDef(DefMap& defs, const Inst& inst) {
  DefInfo info;
  info.op = inst.op;
  info.text = inst.text;
  info.type = inst.type;
  info.operands = inst.operands;
  info.intValue = inst.intValue;
  if (isConstOp(inst.op)) {
    info.isConst = true;
    info.c = constOf(inst);
  }
  defs[inst.id] = std::move(info);
}

Inst makeCopyOf(int id, const Inst& from) {
  Inst copy;
  copy.op = Op::Copy;
  copy.id = from.id;
  copy.type = from.type;
  copy.operands = {id};
  copy.span = from.span;
  return copy;
}

}  // namespace

OptimizationStats instCombine(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    DefMap defs;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts) {
        if (inst.producesValue()) snapshotDef(defs, inst);
      }
    }
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (!inst.producesValue()) continue;
        bool rewritten = false;

        if (inst.op == Op::Binary && inst.operands.size() == 2) {
          const int x = inst.operands[0];
          const int y = inst.operands[1];
          const DefInfo* dx = defOf(defs, x);
          const DefInfo* dy = defOf(defs, y);
          const std::string& op = inst.text;
          if (x == y && dx) {
            const TypeKind kind = kindOf(dx);
            if (isComparison(op) && !isFloatKind(kind) && kind != TypeKind::Error &&
                (!isOrdering(op) || kind == TypeKind::Int || kind == TypeKind::Char ||
                 kind == TypeKind::String)) {
              const bool value = op == "==" || op == "<=" || op == ">=";
              Inst k;
              k.op = Op::ConstBool;
              k.id = inst.id;
              k.type = inst.type;
              k.boolValue = value;
              k.span = inst.span;
              inst = std::move(k);
              rewritten = true;
              ++stats.operationsSimplified;
            } else if ((op == "&" || op == "|") && kind == TypeKind::Int) {
              inst = makeCopyOf(x, inst);
              rewritten = true;
              ++stats.operationsSimplified;
            } else if (op == "^" && kind == TypeKind::Int) {
              Inst k;
              k.op = Op::ConstInt;
              k.id = inst.id;
              k.type = inst.type;
              k.intValue = 0;
              k.span = inst.span;
              inst = std::move(k);
              rewritten = true;
              ++stats.operationsSimplified;
            }
          }
          if (!rewritten && (dx || dy)) {
            // `and`/`or` with a constant bool operand; `==`/`!=` against
            // true/false on bool-typed values.
            const DefInfo* con = nullptr;
            const DefInfo* other = nullptr;
            bool conOnLeft = false;
            if (dx && dx->isConst && dx->c.op == Op::ConstBool) {
              con = dx; other = dy; conOnLeft = true;
            } else if (dy && dy->isConst && dy->c.op == Op::ConstBool) {
              con = dy; other = dx;
            }
            if (con && other && kindOf(other) == TypeKind::Bool) {
              const bool cv = con->c.b;
              const int ov = conOnLeft ? y : x;
              if (op == "and") {
                if (cv) { inst = makeCopyOf(ov, inst); rewritten = true; }
                else {
                  Inst k; k.op = Op::ConstBool; k.id = inst.id; k.type = inst.type;
                  k.boolValue = false; k.span = inst.span; inst = std::move(k); rewritten = true;
                }
              } else if (op == "or") {
                if (cv) {
                  Inst k; k.op = Op::ConstBool; k.id = inst.id; k.type = inst.type;
                  k.boolValue = true; k.span = inst.span; inst = std::move(k); rewritten = true;
                } else { inst = makeCopyOf(ov, inst); rewritten = true; }
              } else if (op == "==" || op == "!=") {
                // x == true -> x ; x == false -> !x (and != inverts).
                const bool negate = (op == "!=") == cv;
                if (!negate) {
                  inst = makeCopyOf(ov, inst);
                } else {
                  inst.op = Op::Unary;
                  inst.text = "!";
                  inst.operands = {ov};
                }
                rewritten = true;
              }
              if (rewritten) ++stats.operationsSimplified;
            }
          }
        } else if (inst.op == Op::Select && inst.operands.size() == 3 &&
                   inst.operands[1] == inst.operands[2]) {
          inst = makeCopyOf(inst.operands[1], inst);
          rewritten = true;
          ++stats.operationsSimplified;
        } else if (inst.op == Op::Unary && inst.operands.size() == 1) {
          const DefInfo* inner = defOf(defs, inst.operands[0]);
          if (inner && inner->op == Op::Unary && inner->operands.size() == 1) {
            const bool notPair = (inst.text == "!" || inst.text == "not") &&
                                 (inner->text == "!" || inner->text == "not");
            if (notPair || (inst.text == inner->text &&
                            (inst.text == "-" || inst.text == "~"))) {
              inst = makeCopyOf(inner->operands[0], inst);
              rewritten = true;
              ++stats.operationsSimplified;
            }
          }
        } else if (inst.op == Op::Copy && inst.operands.size() == 1) {
          const DefInfo* inner = defOf(defs, inst.operands[0]);
          if (inner && inner->op == Op::Copy && inner->operands.size() == 1) {
            inst.operands[0] = inner->operands[0];
            rewritten = true;
            ++stats.copiesPropagated;
          }
        } else if (inst.op == Op::Cast && inst.operands.size() == 1) {
          const int src = inst.operands[0];
          const DefInfo* inner = defOf(defs, src);
          // cast to the same scalar type is a copy.
          if (inner && sameScalarType(inner->type, inst.type)) {
            inst = makeCopyOf(src, inst);
            rewritten = true;
            ++stats.operationsSimplified;
          } else if (inner && inner->op == Op::Cast && inner->operands.size() == 1) {
            const DefInfo* ultimate = defOf(defs, inner->operands[0]);
            std::string kind;
            if (ultimate && combineCastKind(inner->text, inst.text, inner->type,
                                            inst.type, ultimate->type, kind)) {
              inst.text = std::move(kind);
              inst.operands[0] = inner->operands[0];
              rewritten = true;
              ++stats.operationsSimplified;
            }
          }
        } else if ((inst.op == Op::TupleGet || inst.op == Op::ExtractValue) &&
                   inst.operands.size() == 1) {
          const DefInfo* agg = defOf(defs, inst.operands[0]);
          if (agg && agg->op == Op::Tuple &&
              inst.intValue >= 0 &&
              static_cast<std::size_t>(inst.intValue) < agg->operands.size()) {
            inst = makeCopyOf(agg->operands[static_cast<std::size_t>(inst.intValue)], inst);
            rewritten = true;
            ++stats.operationsSimplified;
          }
        } else if (inst.op == Op::InsertValue && inst.operands.size() == 2) {
          const DefInfo* agg = defOf(defs, inst.operands[0]);
          if (agg && agg->op == Op::Tuple && inst.intValue >= 0 &&
              static_cast<std::size_t>(inst.intValue) < agg->operands.size()) {
            // insertvalue(tuple(...), v, i) -> tuple(..., v@i, ...)
            const int inserted = inst.operands[1];
            inst.op = Op::Tuple;
            inst.operands = agg->operands;
            inst.operands[static_cast<std::size_t>(inst.intValue)] = inserted;
            inst.intValue = 0;
            rewritten = true;
            ++stats.operationsSimplified;
          }
        } else if (inst.op == Op::Freeze && inst.operands.size() == 1) {
          const DefInfo* inner = defOf(defs, inst.operands[0]);
          if (inner && inner->op == Op::Freeze) {
            inst = makeCopyOf(inst.operands[0], inst);
            rewritten = true;
            ++stats.operationsSimplified;
          }
        }
        if (rewritten) snapshotDef(defs, inst);
      }
    }
  }
  return stats;
}
}  // namespace ir
}  // namespace a
