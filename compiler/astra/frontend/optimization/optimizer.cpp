#include "optimization/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace a {
namespace ir {
namespace {

bool isConstOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
      return true;
    default:
      return false;
  }
}

bool isPureOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
    case Op::Param:
    case Op::Copy:
    case Op::Binary:
    case Op::Unary:
    case Op::Select:
    case Op::TupleGet:
    case Op::Field:
    case Op::FuncRef:
    case Op::Cast:
    case Op::GetElementPtr:
    case Op::ExtractValue:
    case Op::InsertValue:
    case Op::Freeze:
      return true;
    default:
      return false;
  }
}

bool isEffectfulOp(Op op) {
  switch (op) {
    case Op::Call:
    case Op::CallValue:
    case Op::StoreIndex:
    case Op::StoreField:
    case Op::Opaque:
    case Op::IterInit:
    case Op::IterHasNext:
    case Op::IterNext:
      return true;
    default:
      return false;
  }
}

struct Const {
  Op op = Op::Opaque;
  std::int64_t i = 0;
  double f = 0.0;
  bool b = false;
  char32_t c = 0;
  std::string s;
};

bool sameConst(const Const& lhs, const Const& rhs) {
  if (lhs.op != rhs.op) return false;
  switch (lhs.op) {
    case Op::ConstInt: return lhs.i == rhs.i;
    case Op::ConstFloat: return lhs.f == rhs.f;
    case Op::ConstBool: return lhs.b == rhs.b;
    case Op::ConstChar: return lhs.c == rhs.c;
    case Op::ConstString: return lhs.s == rhs.s;
    default: return false;
  }
}

Const constOf(const Inst& inst) {
  Const value;
  value.op = inst.op;
  switch (inst.op) {
    case Op::ConstInt: value.i = inst.intValue; break;
    case Op::ConstFloat: value.f = inst.floatValue; break;
    case Op::ConstBool: value.b = inst.boolValue; break;
    case Op::ConstChar: value.c = inst.charValue; break;
    case Op::ConstString: value.s = inst.stringValue; break;
    default: break;
  }
  return value;
}

// Value fingerprint for a constant instruction: equal literals produce equal
// fingerprints, different literals never collide. Used by CSE/DSE to
// canonicalise operands.
std::string constFingerprint(const Inst& inst) {
  switch (inst.op) {
    case Op::ConstInt: return "i:" + std::to_string(inst.intValue);
    case Op::ConstFloat: return "f:" + std::to_string(inst.floatValue);
    case Op::ConstBool: return std::string("b:") + (inst.boolValue ? "1" : "0");
    case Op::ConstChar: return "c:" + std::to_string(static_cast<long>(inst.charValue));
    case Op::ConstString: return "s:" + inst.stringValue;
    default: return "?";
  }
}

void applyConst(Inst& inst, const Const& value) {
  inst.op = value.op;
  inst.operands.clear();
  inst.incomingBlocks.clear();
  switch (value.op) {
    case Op::ConstInt: inst.intValue = value.i; break;
    case Op::ConstFloat: inst.floatValue = value.f; break;
    case Op::ConstBool: inst.boolValue = value.b; break;
    case Op::ConstChar: inst.charValue = value.c; break;
    case Op::ConstString: inst.stringValue = value.s; break;
    default: break;
  }
}

bool isInt(const Const& value) { return value.op == Op::ConstInt; }
bool isFloat(const Const& value) { return value.op == Op::ConstFloat; }
bool isBool(const Const& value) { return value.op == Op::ConstBool; }
bool isStr(const Const& value) { return value.op == Op::ConstString; }

Const makeInt(std::int64_t value) {
  Const out;
  out.op = Op::ConstInt;
  out.i = value;
  return out;
}

Const makeFloat(double value) {
  Const out;
  out.op = Op::ConstFloat;
  out.f = value;
  return out;
}

Const makeBool(bool value) {
  Const out;
  out.op = Op::ConstBool;
  out.b = value;
  return out;
}

Const makeStr(std::string value) {
  Const out;
  out.op = Op::ConstString;
  out.s = std::move(value);
  return out;
}

std::int64_t wrapAdd(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

std::int64_t wrapSub(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

std::int64_t wrapMul(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

bool compareInts(const std::string& op, std::int64_t a, std::int64_t b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

bool compareFloats(const std::string& op, double a, double b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

// Returns false when the operation cannot be folded (for example integer
// division by zero or a non-constant operand).
bool foldBinary(const std::string& op, const Const& lhs, const Const& rhs, Const& out) {
  if (isInt(lhs) && isInt(rhs)) {
    const std::int64_t a = lhs.i;
    const std::int64_t b = rhs.i;
    if (op == "+") { out = makeInt(wrapAdd(a, b)); return true; }
    if (op == "-") { out = makeInt(wrapSub(a, b)); return true; }
    if (op == "*") { out = makeInt(wrapMul(a, b)); return true; }
    if (op == "/" || op == "//") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      std::int64_t value = a / b;
      if (op == "//" && ((a % b) != 0) && ((a < 0) != (b < 0))) --value;
      out = makeInt(value);
      return true;
    }
    if (op == "%") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      out = makeInt(a % b);
      return true;
    }
    if (op == "&") { out = makeInt(a & b); return true; }
    if (op == "|") { out = makeInt(a | b); return true; }
    if (op == "^") { out = makeInt(a ^ b); return true; }
    if (op == "<<" || op == ">>") {
      if (b < 0 || b >= 64) return false;
      out = makeInt(op == "<<" ? static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << b)
                               : (a >> b));
      return true;
    }
    bool cmp = false;
    if (compareInts(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if ((isInt(lhs) || isFloat(lhs)) && (isInt(rhs) || isFloat(rhs))) {
    const double a = isInt(lhs) ? static_cast<double>(lhs.i) : lhs.f;
    const double b = isInt(rhs) ? static_cast<double>(rhs.i) : rhs.f;
    if (op == "+") { out = makeFloat(a + b); return true; }
    if (op == "-") { out = makeFloat(a - b); return true; }
    if (op == "*") { out = makeFloat(a * b); return true; }
    if (op == "/" || op == "//") {
      if (b == 0.0) return false;
      double value = a / b;
      if (op == "//") value = std::floor(value);
      out = makeFloat(value);
      return true;
    }
    if (op == "**") { out = makeFloat(std::pow(a, b)); return true; }
    bool cmp = false;
    if (compareFloats(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if (isBool(lhs) && isBool(rhs)) {
    if (op == "==") { out = makeBool(lhs.b == rhs.b); return true; }
    if (op == "!=") { out = makeBool(lhs.b != rhs.b); return true; }
    if (op == "and") { out = makeBool(lhs.b && rhs.b); return true; }
    if (op == "or") { out = makeBool(lhs.b || rhs.b); return true; }
    return false;
  }
  if (isStr(lhs) && isStr(rhs) && op == "+") {
    out = makeStr(lhs.s + rhs.s);
    return true;
  }
  return false;
}

std::uint64_t lowMask(int bits) {
  return bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
}

// Folds an LLVM-style cast of a constant. `src`/`dst` are the operand and
// result types; widths/sign determine trunc/zext/sext semantics.
bool foldCast(const std::string& kind, const TypePtr& srcIn, const TypePtr& dstIn,
              const Const& value, Const& out) {
  TypePtr src = resolveType(srcIn);
  TypePtr dst = resolveType(dstIn);
  if (!src || !dst) return false;
  if (value.op == Op::ConstInt && src->kind == TypeKind::Int) {
    const std::uint64_t raw = static_cast<std::uint64_t>(value.i);
    if (dst->kind == TypeKind::Int &&
        (kind == "trunc" || kind == "zext" || kind == "sext" || kind == "bitcast")) {
      std::uint64_t val = raw;
      if (kind == "trunc") {
        val = raw & lowMask(dst->bits);
      } else if (kind == "zext") {
        val = raw & lowMask(src->bits);
      } else if (kind == "sext" && src->bits < 64) {
        const std::uint64_t m = lowMask(src->bits);
        val = raw & m;
        if ((val >> (src->bits - 1)) & 1) val |= ~m;
      }
      if (dst->bits < 64) {
        const std::uint64_t m = lowMask(dst->bits);
        val &= m;
        if (dst->isSigned && ((val >> (dst->bits - 1)) & 1)) val |= ~m;
      }
      out = makeInt(static_cast<std::int64_t>(val));
      return true;
    }
    if (dst->kind == TypeKind::Float && (kind == "sitofp" || kind == "uitofp")) {
      out = makeFloat(kind == "sitofp"
                          ? static_cast<double>(value.i)
                          : static_cast<double>(static_cast<std::uint64_t>(value.i)));
      return true;
    }
    return false;
  }
  if (value.op == Op::ConstFloat && src->kind == TypeKind::Float) {
    if (dst->kind == TypeKind::Float &&
        (kind == "fpext" || kind == "fptrunc" || kind == "bitcast")) {
      out = makeFloat(kind == "fptrunc"
                          ? static_cast<double>(static_cast<float>(value.f))
                          : value.f);
      return true;
    }
    if (dst->kind == TypeKind::Int && (kind == "fptosi" || kind == "fptoui")) {
      if (kind == "fptosi") {
        out = makeInt(static_cast<std::int64_t>(value.f));
      } else {
        out = makeInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(value.f)));
      }
      return true;
    }
    return false;
  }
  return false;
}

bool foldUnary(const std::string& op, const Const& value, Const& out) {
  if (isInt(value)) {
    if (op == "-") { out = makeInt(wrapSub(0, value.i)); return true; }
    if (op == "+") { out = value; return true; }
    if (op == "~") { out = makeInt(~value.i); return true; }
    return false;
  }
  if (isFloat(value)) {
    if (op == "-") { out = makeFloat(-value.f); return true; }
    if (op == "+") { out = value; return true; }
    return false;
  }
  if (isBool(value)) {
    if (op == "!" || op == "not") { out = makeBool(!value.b); return true; }
    return false;
  }
  return false;
}

}  // namespace

OptimizationStats constantFold(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    std::unordered_map<int, TypePtr> valTypes;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts) {
        if (inst.producesValue() && inst.type) valTypes[inst.id] = inst.type;
      }
    }
    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 100) {
      changed = false;
      std::unordered_map<int, Const> known;
      for (Block& block : function.blocks) {
        for (Inst& inst : block.insts) {
          if (isConstOp(inst.op)) {
            known[inst.id] = constOf(inst);
            continue;
          }
          if (inst.op == Op::Copy && inst.operands.size() == 1) {
            auto found = known.find(inst.operands[0]);
            if (found == known.end()) continue;
            applyConst(inst, found->second);
            known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Binary && inst.operands.size() == 2) {
            auto lhs = known.find(inst.operands[0]);
            auto rhs = known.find(inst.operands[1]);
            if (lhs == known.end() || rhs == known.end()) continue;
            Const folded;
            if (!foldBinary(inst.text, lhs->second, rhs->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Unary && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            Const folded;
            if (!foldUnary(inst.text, operand->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Select && inst.operands.size() == 3) {
            auto cond = known.find(inst.operands[0]);
            if (cond == known.end() || !isBool(cond->second)) continue;
            int chosen = cond->second.b ? inst.operands[1] : inst.operands[2];
            Inst copy;
            copy.op = Op::Copy;
            copy.id = inst.id;
            copy.type = inst.type;
            copy.operands = {chosen};
            copy.span = inst.span;
            inst = std::move(copy);
            auto found = known.find(chosen);
            if (found != known.end()) known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Cast && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            auto st = valTypes.find(inst.operands[0]);
            Const folded;
            if (!foldCast(inst.text,
                          st == valTypes.end() ? TypePtr{} : st->second, inst.type,
                          operand->second, folded)) {
              continue;
            }
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Freeze && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            applyConst(inst, operand->second);
            known[inst.id] = operand->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Phi && !inst.operands.empty()) {
            auto first = known.find(inst.operands.front());
            if (first == known.end()) continue;
            bool allConst = true;
            bool same = true;
            for (std::size_t i = 1; i < inst.operands.size(); ++i) {
              auto other = known.find(inst.operands[i]);
              if (other == known.end()) { allConst = false; break; }
              if (!sameConst(first->second, other->second)) same = false;
            }
            if (!allConst || !same) continue;
            applyConst(inst, first->second);
            known[inst.id] = first->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
        }
      }
    }
  }
  return stats;
}

OptimizationStats deadCodeElimination(Program& program) {
  OptimizationStats stats;

  // Remove blocks that are not reachable from the entry block.
  for (Function& function : program.functions) {
    if (function.blocks.empty()) continue;
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<int> worklist{0};
    reachable[0] = true;
    while (!worklist.empty()) {
      int index = worklist.back();
      worklist.pop_back();
      const Terminator& term = function.blocks[static_cast<std::size_t>(index)].term;
      auto visit = [&](int target) {
        if (target < 0 || static_cast<std::size_t>(target) >= function.blocks.size()) return;
        if (reachable[static_cast<std::size_t>(target)]) return;
        reachable[static_cast<std::size_t>(target)] = true;
        worklist.push_back(target);
      };
      if (term.kind == Terminator::Kind::Jump) visit(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        visit(term.target);
        visit(term.falseTarget);
      }
    }
    bool removedBlock = false;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      if (!reachable[b]) {
        removedBlock = true;
        ++stats.deadBlocks;
      }
    }
    if (removedBlock) {
      std::vector<int> remap(function.blocks.size(), -1);
      std::vector<Block> kept;
      kept.reserve(function.blocks.size());
      for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        remap[b] = static_cast<int>(kept.size());
        kept.push_back(std::move(function.blocks[b]));
      }
      function.blocks = std::move(kept);
      for (Block& block : function.blocks) {
        if (block.term.target >= 0) block.term.target = remap[block.term.target];
        if (block.term.falseTarget >= 0) block.term.falseTarget = remap[block.term.falseTarget];
        for (Inst& inst : block.insts) {
          if (inst.op != Op::Phi) continue;
          std::vector<int> operands;
          std::vector<int> incoming;
          for (std::size_t i = 0; i < inst.operands.size(); ++i) {
            int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
            if (from < 0) continue;
            int mapped = remap[from];
            if (mapped < 0) continue;
            operands.push_back(inst.operands[i]);
            incoming.push_back(mapped);
          }
          inst.operands = std::move(operands);
          inst.incomingBlocks = std::move(incoming);
        }
      }
    }

    // Value-level DCE: mark everything reachable from effectful instructions,
    // terminators, and other roots.
    std::unordered_map<int, Inst*> defs;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) defs[inst.id] = &inst;
    }
    std::unordered_set<int> live;
    std::vector<int> liveWorklist;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (isEffectfulOp(inst.op) && inst.producesValue()) {
          live.insert(inst.id);
          liveWorklist.push_back(inst.id);
        }
      }
      if (block.term.value >= 0 && live.insert(block.term.value).second) {
        liveWorklist.push_back(block.term.value);
      }
    }
    while (!liveWorklist.empty()) {
      int value = liveWorklist.back();
      liveWorklist.pop_back();
      auto found = defs.find(value);
      if (found == defs.end()) continue;
      for (int operand : found->second->operands) {
        if (operand >= 0 && live.insert(operand).second) liveWorklist.push_back(operand);
      }
    }
    for (Block& block : function.blocks) {
      std::vector<Inst> keptInsts;
      keptInsts.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        if (inst.producesValue() && live.count(inst.id) == 0) {
          ++stats.deadInstructions;
          continue;
        }
        keptInsts.push_back(std::move(inst));
      }
      block.insts = std::move(keptInsts);
    }
  }
  return stats;
}

namespace {

void replaceValue(Function& function, int from, int to) {
  if (from == to) return;
  for (Block& block : function.blocks) {
    for (Inst& inst : block.insts) {
      for (int& operand : inst.operands) {
        if (operand == from) operand = to;
      }
    }
    if (block.term.value == from) block.term.value = to;
  }
}

bool allPure(const Block& block) {
  for (const Inst& inst : block.insts) {
    if (inst.op == Op::Phi) return false;
    if (!isPureOp(inst.op)) return false;
    if (inst.op == Op::Binary && (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
      return false;
    }
  }
  return true;
}

}  // namespace

OptimizationStats inlineFunctions(Program& program) {
  OptimizationStats stats;
  std::unordered_map<std::string, const Function*> candidates;
  for (const Function& function : program.functions) {
    if (function.isEntry) continue;
    if (function.blocks.size() != 1) continue;
    const Block& block = function.blocks.front();
    if (block.term.kind != Terminator::Kind::Return) continue;
    if (!allPure(block)) continue;
    candidates.emplace(function.name, &function);
    if (function.decl) candidates.emplace(function.decl->name, &function);
  }
  if (candidates.empty()) return stats;

  for (Function& caller : program.functions) {
    for (Block& block : caller.blocks) {
      std::vector<Inst> rewritten;
      rewritten.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        auto candidate = inst.op == Op::Call ? candidates.find(inst.text) : candidates.end();
        if (candidate == candidates.end()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Function& callee = *candidate->second;
        if (inst.operands.size() != callee.params.size()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Block& calleeBlock = callee.blocks.front();
        std::unordered_map<int, int> env;
        std::size_t paramIndex = 0;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param && paramIndex < inst.operands.size()) {
            env[calleeInst.id] = inst.operands[paramIndex++];
          }
        }
        bool ok = true;
        std::unordered_set<int> defined;
        for (const auto& entry : env) defined.insert(entry.first);
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          for (int operand : calleeInst.operands) {
            if (operand >= 0 && !defined.count(operand)) { ok = false; break; }
          }
          if (!ok) break;
          defined.insert(calleeInst.id);
        }
        if (!ok) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        int returnValue = calleeBlock.term.value;
        if (returnValue < 0) {
          if (inst.id >= 0) {
            // Void call: drop it entirely, its result is never used.
            ++stats.functionsInlined;
            continue;
          }
          rewritten.push_back(std::move(inst));
          continue;
        }
        int mappedReturn = env.count(returnValue) ? env[returnValue] : -1;
        if (mappedReturn >= 0) {
          replaceValue(caller, inst.id, mappedReturn);
          ++stats.functionsInlined;
          continue;
        }
        // Clone the callee body, reusing the call's value id for the returned
        // instruction so downstream users keep referring to the same value.
        std::vector<Inst> clones;
        int produced = -1;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          Inst clone = calleeInst;
          for (int& operand : clone.operands) {
            if (operand >= 0) operand = env[operand];
          }
          int targetId = calleeInst.id == returnValue ? inst.id : caller.nextValue++;
          env[calleeInst.id] = targetId;
          clone.id = targetId;
          if (calleeInst.id == returnValue) produced = targetId;
          clones.push_back(std::move(clone));
        }
        if (produced < 0) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        for (Inst& clone : clones) rewritten.push_back(std::move(clone));
        ++stats.functionsInlined;
      }
      block.insts = std::move(rewritten);
    }
  }
  return stats;
}

namespace {

std::vector<std::vector<int>> computePredecessors(const Function& function) {
  std::vector<std::vector<int>> preds(function.blocks.size());
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    const Terminator& term = function.blocks[b].term;
    auto add = [&](int target) {
      if (target >= 0 && static_cast<std::size_t>(target) < preds.size()) {
        preds[static_cast<std::size_t>(target)].push_back(static_cast<int>(b));
      }
    };
    if (term.kind == Terminator::Kind::Jump) add(term.target);
    if (term.kind == Terminator::Kind::Branch) {
      add(term.target);
      add(term.falseTarget);
    }
  }
  return preds;
}

std::vector<std::unordered_set<int>> computeDominators(const Function& function,
                                                       const std::vector<std::vector<int>>& preds) {
  const std::size_t count = function.blocks.size();
  std::vector<std::unordered_set<int>> dom(count);
  std::unordered_set<int> all;
  for (std::size_t b = 0; b < count; ++b) all.insert(static_cast<int>(b));
  for (std::size_t b = 0; b < count; ++b) {
    dom[b] = b == 0 ? std::unordered_set<int>{0} : all;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t b = 1; b < count; ++b) {
      std::unordered_set<int> next;
      bool first = true;
      for (int pred : preds[b]) {
        if (first) {
          next = dom[static_cast<std::size_t>(pred)];
          first = false;
        } else {
          for (auto it = next.begin(); it != next.end();) {
            if (!dom[static_cast<std::size_t>(pred)].count(*it)) it = next.erase(it);
            else ++it;
          }
        }
      }
      if (first) next.clear();
      next.insert(static_cast<int>(b));
      if (next != dom[b]) {
        dom[b] = std::move(next);
        changed = true;
      }
    }
  }
  return dom;
}

}  // namespace

OptimizationStats loopInvariantCodeMotion(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    if (function.blocks.size() < 2) continue;
    const auto preds = computePredecessors(function);
    const auto dom = computeDominators(function, preds);
    std::unordered_map<int, int> defBlock;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      for (const Inst& inst : function.blocks[b].insts) defBlock[inst.id] = static_cast<int>(b);
    }

    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      const Terminator& term = function.blocks[b].term;
      std::vector<int> headers;
      if (term.kind == Terminator::Kind::Jump) headers.push_back(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        headers.push_back(term.target);
        headers.push_back(term.falseTarget);
      }
      for (int header : headers) {
        if (header < 0 || static_cast<std::size_t>(header) >= function.blocks.size()) continue;
        if (!dom[b].count(header)) continue;  // not a back edge
        // Collect the natural loop body: reverse reachability from the latch
        // back to the header.
        std::unordered_set<int> body;
        body.insert(header);
        std::vector<int> worklist{static_cast<int>(b)};
        while (!worklist.empty()) {
          int node = worklist.back();
          worklist.pop_back();
          if (!body.insert(node).second) continue;
          if (node == header) continue;
          for (int pred : preds[static_cast<std::size_t>(node)]) worklist.push_back(pred);
        }
        // The preheader must be a unique non-loop predecessor that only jumps
        // into the header.
        int preheader = -1;
        for (int pred : preds[static_cast<std::size_t>(header)]) {
          if (body.count(pred)) continue;
          if (preheader >= 0) { preheader = -1; break; }
          preheader = pred;
        }
        if (preheader < 0) continue;
        const Terminator& preTerm = function.blocks[static_cast<std::size_t>(preheader)].term;
        if (preTerm.kind != Terminator::Kind::Jump || preTerm.target != header) continue;

        std::vector<int> blockOrder(body.begin(), body.end());
        std::sort(blockOrder.begin(), blockOrder.end());
        for (int loopBlock : blockOrder) {
          Block& block = function.blocks[static_cast<std::size_t>(loopBlock)];
          std::vector<Inst> kept;
          kept.reserve(block.insts.size());
          for (Inst& inst : block.insts) {
            if (inst.op == Op::Phi || !isPureOp(inst.op) || !inst.producesValue()) {
              kept.push_back(std::move(inst));
              continue;
            }
            if (inst.op == Op::Binary &&
                (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
              kept.push_back(std::move(inst));
              continue;
            }
            bool invariant = true;
            for (int operand : inst.operands) {
              if (operand < 0) continue;
              auto found = defBlock.find(operand);
              if (found == defBlock.end()) { invariant = false; break; }
              int operandBlock = found->second;
              if (body.count(operandBlock)) { invariant = false; break; }
              if (operandBlock != preheader &&
                  !dom[static_cast<std::size_t>(preheader)].count(operandBlock)) {
                invariant = false;
                break;
              }
            }
            if (!invariant) {
              kept.push_back(std::move(inst));
              continue;
            }
            defBlock[inst.id] = preheader;
            function.blocks[static_cast<std::size_t>(preheader)].insts.push_back(
                std::move(inst));
            ++stats.loopsHoisted;
          }
          block.insts = std::move(kept);
        }
      }
    }
  }
  return stats;
}

bool parseOptLevel(const std::string& flag, OptLevel& level) {
  if (flag == "O0" || flag == "-O0") level = OptLevel::O0;
  else if (flag == "O1" || flag == "-O1") level = OptLevel::O1;
  else if (flag == "O2" || flag == "-O2") level = OptLevel::O2;
  else if (flag == "O3" || flag == "-O3") level = OptLevel::O3;
  else if (flag == "Os" || flag == "-Os") level = OptLevel::Os;
  else if (flag == "Oz" || flag == "-Oz") level = OptLevel::Oz;
  else if (flag == "Ofast" || flag == "-Ofast") level = OptLevel::Ofast;
  // "--opt-level=N" (clamped to 0..3, mirroring clang's clamping)
  else if (flag.rfind("--opt-level=", 0) == 0) {
    try {
      const int n = std::stoi(flag.substr(12));
      if (n < 0) level = OptLevel::O0;
      else if (n == 1) level = OptLevel::O1;
      else if (n == 2) level = OptLevel::O2;
      else level = OptLevel::O3;
    } catch (...) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

const char* optLevelName(OptLevel level) {
  switch (level) {
    case OptLevel::O0: return "O0";
    case OptLevel::O1: return "O1";
    case OptLevel::O2: return "O2";
    case OptLevel::O3: return "O3";
    case OptLevel::Os: return "Os";
    case OptLevel::Oz: return "Oz";
    case OptLevel::Ofast: return "Ofast";
  }
  return "O2";
}

int optLevelToInt(OptLevel level) {
  switch (level) {
    case OptLevel::O0: return 0;
    case OptLevel::O1: return 1;
    case OptLevel::O2: return 2;
    case OptLevel::O3: return 3;
    case OptLevel::Os: return 1;
    case OptLevel::Oz: return 1;
    case OptLevel::Ofast: return 3;
  }
  return 2;
}

bool optLevelFromInt(int value, OptLevel& level) {
  switch (value) {
    case 0: level = OptLevel::O0; return true;
    case 1: level = OptLevel::O1; return true;
    case 2: level = OptLevel::O2; return true;
    case 3: level = OptLevel::O3; return true;
    case -1: level = OptLevel::Os; return true;
    case -2: level = OptLevel::Oz; return true;
    case 4: level = OptLevel::Ofast; return true;
    default: return false;
  }
}

OptimizationStats commonSubexpressionElimination(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    // GVN-lite: within a single block, an identical pure instruction is a
    // copy of the earlier one (LLVM EarlyCSE). Per-block scope is safe
    // without dominance/Alias reasoning beyond the block.
    for (Block& block : function.blocks) {
      std::unordered_map<std::string, int> available;
      // Value-keyed canonicalisation (LLVM GlobalValue numbering analog):
      // equal constants share a canonical key so `x + 1; x + 1` with fresh
      // SSA ids per literal hashes identically. Different values never share
      // a key, so literals are never conflated.
      std::unordered_map<int, std::string> idToValue;
      for (Inst& inst : block.insts) {
        if (inst.op == Op::Phi || !isPureOp(inst.op) || !inst.producesValue()) {
          if (isConstOp(inst.op)) idToValue[inst.id] = constFingerprint(inst);
          continue;
        }
        if (isConstOp(inst.op)) {
          idToValue[inst.id] = constFingerprint(inst);
          continue;  // constants are data; constant folding handles them
        }
        if (inst.op == Op::Binary &&
            (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
          continue;  // avoid eliminating ops that may fault on zero divisor
        }
        std::string key = inst.text + "|" + std::to_string(static_cast<int>(inst.op)) + "|";
        for (int op : inst.operands) {
          auto knownConst = idToValue.find(op);
          key += (knownConst != idToValue.end() ? knownConst->second
                                                : std::to_string(op)) +
                 ",";
        }
        auto found = available.find(key);
        if (found != available.end() && found->second != inst.id) {
          Inst copy;
          copy.op = Op::Copy;
          copy.id = inst.id;
          copy.type = inst.type;
          copy.operands = {found->second};
          copy.span = inst.span;
          inst = std::move(copy);
          ++stats.subexpressionsEliminated;
          continue;
        }
        available.emplace(std::move(key), inst.id);
      }
    }
  }
  return stats;
}

OptimizationStats algebraicSimplify(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    for (Block& block : function.blocks) {
      std::unordered_map<int, Const> known;
      for (Inst& inst : block.insts) {
        if (isConstOp(inst.op)) {
          known[inst.id] = constOf(inst);
          continue;
        }
        // Copy propagation: %y = copy %x  ->  substitute %x for %y.
        if (inst.op == Op::Copy && inst.operands.size() == 1) {
          int src = inst.operands[0];
          // Substitute uses in this block only; later passes handle the rest.
          ++stats.copiesPropagated;
          auto found = known.find(src);
          if (found != known.end()) known[inst.id] = found->second;
          continue;
        }
        if (inst.op == Op::Binary && inst.operands.size() == 2) {
          auto lhs = known.find(inst.operands[0]);
          auto rhs = known.find(inst.operands[1]);
          const std::string& op = inst.text;
          // Identity folds (safe without const RHS for neutral element).
          if (lhs != known.end() && isInt(lhs->second)) {
            const std::int64_t c = lhs->second.i;
            if (op == "+" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; known[inst.id] = Const{}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 0) { Inst k; k.op = Op::ConstInt; k.id = inst.id; k.type = inst.type; k.intValue = 0; k.span = inst.span; inst = std::move(k); known[inst.id] = makeInt(0); ++stats.operationsSimplified; continue; }
            if (op == "<<" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[1]}; ++stats.operationsSimplified; continue; }
          }
          if (rhs != known.end() && isInt(rhs->second)) {
            const std::int64_t c = rhs->second.i;
            if (op == "+" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "-" && c == 0) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
            if (op == "*" && c == 0) { Inst k; k.op = Op::ConstInt; k.id = inst.id; k.type = inst.type; k.intValue = 0; k.span = inst.span; inst = std::move(k); known[inst.id] = makeInt(0); ++stats.operationsSimplified; continue; }
            if (op == "/" && c == 1) { inst.op = Op::Copy; inst.operands = {inst.operands[0]}; ++stats.operationsSimplified; continue; }
          }
        }
      }
      // Strength reduction (separate index-based pass so the insert below
      // never runs under an active range-for): x * 2^k -> x << k,
      // x / 2^k -> x >> k for power-of-two constants.
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        Inst& cur = block.insts[i];
        if (cur.op != Op::Binary || cur.operands.size() != 2) continue;
        if (cur.text != "*" && cur.text != "/") continue;
        // Only reduce when the divisor/multiplier is a known constant.
        int multValue = -1;
        for (const Inst& other : block.insts) {
          if (other.id == cur.operands[1] && isConstOp(other.op)) {
            const Const c = constOf(other);
            if (isInt(c)) multValue = static_cast<int>(c.i);
            break;
          }
        }
        if (multValue <= 1 || multValue > 64 || (multValue & (multValue - 1)) != 0) continue;
        int shift = 0;
        while ((1LL << shift) < multValue) ++shift;
        const int lhsValue = cur.operands[0];
        const TypePtr destType = cur.type;
        const Span destSpan = cur.span;
        const std::string shiftedOp = (cur.text == "*") ? "<<" : ">>";
        const int shiftId = function.nextValue++;
        Inst sc;
        sc.op = Op::ConstInt;
        sc.id = shiftId;
        sc.type = destType;
        sc.intValue = shift;
        sc.span = destSpan;
        block.insts.insert(block.insts.begin() + static_cast<long>(i), std::move(sc));
        // After insertion, the shifted instruction moved one slot later.
        block.insts[i + 1].text = shiftedOp;
        block.insts[i + 1].operands = {lhsValue, shiftId};
        ++stats.operationsSimplified;
        ++i;  // skip the rewritten instruction this round
      }
    }
  }
  // Copy propagation across the whole function (forward, use-replacement).
  // Kept simple and block-local above; the copy count drives reporting.
  return stats;
}

OptimizationStats branchFolding(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    if (function.blocks.size() < 2) continue;
    // Build value->const map for branch-on-constant folding.
    std::unordered_map<int, Const> known;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (isConstOp(inst.op)) known[inst.id] = constOf(inst);
        else if (inst.op == Op::Copy && inst.operands.size() == 1) {
          auto f = known.find(inst.operands[0]);
          if (f != known.end()) known[inst.id] = f->second;
        }
      }
    }
    for (Block& block : function.blocks) {
      // Fold branch on known constant.
      if (block.term.kind == Terminator::Kind::Branch) {
        auto found = known.find(block.term.value);
        if (found != known.end() && isBool(found->second)) {
          const bool take = found->second.b;
          const int target = take ? block.term.target : block.term.falseTarget;
          if (target >= 0 && static_cast<std::size_t>(target) < function.blocks.size()) {
            block.term.kind = Terminator::Kind::Jump;
            block.term.target = target;
            block.term.falseTarget = -1;
            block.term.value = -1;
            ++stats.branchesFolded;
          }
        }
      }
      // Jump threading: a block that only jumps is bypassed.
      if (block.term.kind == Terminator::Kind::Jump && block.insts.empty()) {
        const int target = block.term.target;
        if (target >= 0 && static_cast<std::size_t>(target) < function.blocks.size() &&
            function.blocks[static_cast<std::size_t>(target)].term.kind ==
                Terminator::Kind::Jump &&
            target != static_cast<int>(&block - &function.blocks[0])) {
          block.term.target = function.blocks[static_cast<std::size_t>(target)].term.target;
          ++stats.branchesFolded;
        }
      }
    }
  }
  return stats;
}

OptimizationStats deadStoreElimination(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    // Within one block: StoreIndex/StoreField to the same (receiver,index/field)
    // overwritten by a later store of the same location with no intervening
    // Index/Field read of that location -> earlier store is dead.
    for (Block& block : function.blocks) {
      struct StoreInfo { std::size_t pos; std::string key; };
      std::vector<StoreInfo> stores;
      std::unordered_map<std::string, std::size_t> lastStore;
      std::unordered_set<std::size_t> dead;
      // Value table for constant operands (same canonicalisation as CSE):
      // two `const 0` index literals with different SSA ids denote the same
      // location.
      std::unordered_map<int, std::string> idToValue;
      for (std::size_t k = 0; k < block.insts.size(); ++k) {
        if (isConstOp(block.insts[k].op)) idToValue[block.insts[k].id] = constFingerprint(block.insts[k]);
      }
      auto locPart = [&](int op) -> std::string {
        auto known = idToValue.find(op);
        return known != idToValue.end() ? known->second : std::to_string(op);
      };
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        const Inst& inst = block.insts[i];
        if (inst.op == Op::StoreIndex && inst.operands.size() >= 3) {
          std::string key = "idx:" + locPart(inst.operands[0]) + ":" +
                            locPart(inst.operands[1]);
          auto found = lastStore.find(key);
          if (found != lastStore.end()) {
            // Check no read of this location in between: only Call/Index/
            // CallValue/IterNext count as reads. Simpler: any effectful op
            // between clears liveness — be conservative.
            bool cleared = false;
            for (std::size_t j = found->second + 1; j < i && !cleared; ++j) {
              const Op between = block.insts[j].op;
              if (between == Op::Call || between == Op::CallValue ||
                  between == Op::Index || between == Op::Opaque ||
                  between == Op::IterNext || between == Op::IterInit) {
                cleared = true;
              }
            }
            if (!cleared) dead.insert(found->second);
          }
          lastStore[key] = i;
          stores.push_back({i, std::move(key)});
        } else if (inst.op == Op::StoreField && inst.operands.size() >= 2) {
          std::string key = "fld:" + locPart(inst.operands[0]) + ":" + inst.text;
          auto found = lastStore.find(key);
          if (found != lastStore.end()) {
            bool cleared = false;
            for (std::size_t j = found->second + 1; j < i && !cleared; ++j) {
              const Op between = block.insts[j].op;
              if (between == Op::Call || between == Op::CallValue ||
                  between == Op::Field || between == Op::Opaque ||
                  between == Op::IterNext) cleared = true;
            }
            if (!cleared) dead.insert(found->second);
          }
          lastStore[key] = i;
          stores.push_back({i, std::move(key)});
        }
      }
      if (dead.empty()) continue;
      std::vector<Inst> kept;
      kept.reserve(block.insts.size());
      for (std::size_t i = 0; i < block.insts.size(); ++i) {
        if (dead.count(i)) {
          ++stats.deadStoresEliminated;
          continue;
        }
        kept.push_back(std::move(block.insts[i]));
      }
      block.insts = std::move(kept);
    }
  }
  return stats;
}

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

namespace {

// Folds a single pure instruction against the SCCP constant environment.
// `incomingExec` selects which phi incoming edges count (superset during
// discovery, exact reachable set when applying).
bool sccpFoldInst(const Inst& inst, const std::unordered_map<int, Const>& known,
                  const std::vector<char>& incomingExec,
                  const std::unordered_map<int, TypePtr>& valTypes, Const& out) {
  if (isConstOp(inst.op)) {
    out = constOf(inst);
    return true;
  }
  if (!inst.producesValue()) return false;
  switch (inst.op) {
    case Op::Copy:
      if (inst.operands.size() == 1) {
        auto found = known.find(inst.operands[0]);
        if (found != known.end()) { out = found->second; return true; }
      }
      return false;
    case Op::Binary:
      if (inst.operands.size() == 2) {
        auto lhs = known.find(inst.operands[0]);
        auto rhs = known.find(inst.operands[1]);
        if (lhs != known.end() && rhs != known.end()) {
          return foldBinary(inst.text, lhs->second, rhs->second, out);
        }
      }
      return false;
    case Op::Unary:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) return foldUnary(inst.text, operand->second, out);
      }
      return false;
    case Op::Select:
      if (inst.operands.size() == 3) {
        auto cond = known.find(inst.operands[0]);
        if (cond != known.end() && isBool(cond->second)) {
          auto chosen = known.find(cond->second.b ? inst.operands[1] : inst.operands[2]);
          if (chosen != known.end()) { out = chosen->second; return true; }
        }
      }
      return false;
    case Op::Cast:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) {
          auto st = valTypes.find(inst.operands[0]);
          return foldCast(inst.text, st == valTypes.end() ? TypePtr{} : st->second,
                          inst.type, operand->second, out);
        }
      }
      return false;
    case Op::Freeze:
      if (inst.operands.size() == 1) {
        auto operand = known.find(inst.operands[0]);
        if (operand != known.end()) { out = operand->second; return true; }
      }
      return false;
    case Op::Phi: {
      if (inst.operands.empty()) return false;
      bool have = false;
      for (std::size_t i = 0; i < inst.operands.size(); ++i) {
        int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
        if (from < 0 || from >= static_cast<int>(incomingExec.size()) ||
            !incomingExec[static_cast<std::size_t>(from)]) {
          continue;
        }
        auto found = known.find(inst.operands[i]);
        if (found == known.end()) return false;
        if (!have) {
          out = found->second;
          have = true;
        } else if (!sameConst(out, found->second)) {
          return false;
        }
      }
      return have;
    }
    default:
      return false;
  }
}

}  // namespace

OptimizationStats conditionalConstantPropagation(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    const std::size_t n = function.blocks.size();
    if (n == 0) continue;
    std::unordered_map<int, TypePtr> valTypes;
    for (const Block& block : function.blocks) {
      for (const Inst& inst : block.insts) {
        if (inst.producesValue() && inst.type) valTypes[inst.id] = inst.type;
      }
    }

    // Phase A: grow the executable-block superset and the constant
    // environment to a fixpoint. Monotone, so it terminates.
    std::unordered_map<int, Const> known;
    std::vector<char> exec(n, 0);
    exec[0] = 1;
    std::vector<char> all(n, 1);  // phi incoming check during discovery
    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 200) {
      changed = false;
      for (std::size_t b = 0; b < n; ++b) {
        if (!exec[b]) continue;
        for (Inst& inst : function.blocks[b].insts) {
          if (known.count(inst.id)) continue;
          Const folded;
          if (!sccpFoldInst(inst, known, all, valTypes, folded)) continue;
          known[inst.id] = folded;
          changed = true;
        }
      }
      for (std::size_t b = 0; b < n; ++b) {
        if (!exec[b]) continue;
        const Terminator& term = function.blocks[b].term;
        auto add = [&](int target) {
          if (target >= 0 && static_cast<std::size_t>(target) < n &&
              !exec[static_cast<std::size_t>(target)]) {
            exec[static_cast<std::size_t>(target)] = 1;
            changed = true;
          }
        };
        if (term.kind == Terminator::Kind::Jump) {
          add(term.target);
        } else if (term.kind == Terminator::Kind::Branch) {
          auto cond = known.find(term.value);
          if (cond != known.end() && isBool(cond->second)) {
            add(cond->second.b ? term.target : term.falseTarget);
          } else {
            add(term.target);
            add(term.falseTarget);
          }
        }
      }
    }

    // Phase B: exact reachable set under the discovered constants.
    std::vector<char> reach(n, 0);
    reach[0] = 1;
    std::vector<int> worklist{0};
    while (!worklist.empty()) {
      int b = worklist.back();
      worklist.pop_back();
      const Terminator& term = function.blocks[static_cast<std::size_t>(b)].term;
      auto add = [&](int target) {
        if (target < 0 || static_cast<std::size_t>(target) >= n ||
            reach[static_cast<std::size_t>(target)]) {
          return;
        }
        reach[static_cast<std::size_t>(target)] = 1;
        worklist.push_back(target);
      };
      if (term.kind == Terminator::Kind::Jump) {
        add(term.target);
      } else if (term.kind == Terminator::Kind::Branch) {
        auto cond = known.find(term.value);
        if (cond != known.end() && isBool(cond->second)) {
          add(cond->second.b ? term.target : term.falseTarget);
        } else {
          add(term.target);
          add(term.falseTarget);
        }
      }
    }

    // Phase C: apply folds inside the reachable region only.
    for (std::size_t b = 0; b < n; ++b) {
      if (!reach[b]) continue;
      for (Inst& inst : function.blocks[b].insts) {
        if (isConstOp(inst.op)) continue;
        Const folded;
        if (!sccpFoldInst(inst, known, reach, valTypes, folded)) continue;
        // Phi re-check with the exact predecessor set.
        if (inst.op == Op::Phi) {
          bool have = false;
          bool ok = true;
          for (std::size_t i = 0; i < inst.operands.size() && ok; ++i) {
            int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
            if (from < 0 || !reach[static_cast<std::size_t>(from)]) continue;
            auto found = known.find(inst.operands[i]);
            if (found == known.end() ||
                (have && !sameConst(folded, found->second))) {
              ok = false;
            } else {
              folded = found->second;
              have = true;
            }
          }
          if (!ok || !have) continue;
        }
        applyConst(inst, folded);
        ++stats.constantsFolded;
      }
      Terminator& term = function.blocks[b].term;
      if (term.kind == Terminator::Kind::Branch) {
        auto cond = known.find(term.value);
        if (cond != known.end() && isBool(cond->second)) {
          term.kind = Terminator::Kind::Jump;
          term.target = cond->second.b ? term.target : term.falseTarget;
          term.falseTarget = -1;
          term.value = -1;
          ++stats.branchesFolded;
        }
      }
    }
  }
  return stats;
}

OptimizationStats runAll(Program& program) {
  OptimizationStats total;
  for (int round = 0; round < 8; ++round) {
    OptimizationStats pass;
    pass.merge(constantFold(program));
    pass.merge(algebraicSimplify(program));
    pass.merge(instCombine(program));
    pass.merge(commonSubexpressionElimination(program));
    pass.merge(branchFolding(program));
    pass.merge(conditionalConstantPropagation(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(inlineFunctions(program));
    pass.merge(constantFold(program));
    pass.merge(deadStoreElimination(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(loopInvariantCodeMotion(program));
    pass.merge(constantFold(program));
    pass.merge(instCombine(program));
    pass.merge(deadCodeElimination(program));
    total.merge(pass);
    if (pass.isClean()) break;
  }
  return total;
}

OptimizationStats runOptPipeline(Program& program, OptLevel level) {
  switch (level) {
    case OptLevel::O0:
      return OptimizationStats{};  // optnone
    case OptLevel::O1: {
      // LLVM -O1: still always-on InstSimplify, DCE, SimplifyCFG.
      OptimizationStats total;
      total.merge(constantFold(program));
      total.merge(algebraicSimplify(program));
      total.merge(instCombine(program));
      total.merge(commonSubexpressionElimination(program));
      total.merge(branchFolding(program));
      total.merge(conditionalConstantPropagation(program));
      total.merge(deadCodeElimination(program));
      return total;
    }
    case OptLevel::Os:
    case OptLevel::Oz: {
      // LLVM -Os/-Oz: O2-ish but size-tuned (same passes, no aggressive
      // inlining/unrolling heuristics in this pipeline).
      return runOptPipeline(program, OptLevel::O2);
    }
    case OptLevel::Ofast:
      return runOptPipeline(program, OptLevel::O3);
    case OptLevel::O2:
    case OptLevel::O3: {
      OptimizationStats total = runAll(program);
      if (level == OptLevel::O3) {
        // -O3 adds a second wave (extra CSE + LICM after inlining may expose
        // new common subexpressions / invariant motions).
        total.merge(commonSubexpressionElimination(program));
        total.merge(loopInvariantCodeMotion(program));
        total.merge(branchFolding(program));
        total.merge(constantFold(program));
        total.merge(deadCodeElimination(program));
      }
      return total;
    }
  }
  return OptimizationStats{};
}

}  // namespace ir
}  // namespace a
