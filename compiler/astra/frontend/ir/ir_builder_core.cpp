#include "ir/ir_internal.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "name-resolution/module_loader.hpp"
#include "strings/utf8.hpp"
#include "type-checker/sema.hpp"

namespace a::ir::detail {

void Builder::lowerParameters() {
    int entry = addBlock("entry");
    current_ = entry;
    for (auto& param : fn_.params) {
      Inst inst;
      inst.op = Op::Param;
      inst.type = param.type;
      inst.text = param.name;
      int value = emit(inst);
      param.value = value;
      scopes_.back()[param.name] = value;
    }
  }

void Builder::lowerBody(const std::vector<StmtPtr>& body) {
    lowerStatements(body);
    finish();
  }

void Builder::lowerTopLevel(const std::vector<StmtPtr>& statements) {
    for (const auto& stmt : statements) {
      if (terminated_) break;
      if (stmt->kind == StmtKind::FnDecl || stmt->kind == StmtKind::StructDecl ||
          stmt->kind == StmtKind::EnumDecl || stmt->kind == StmtKind::TypeAlias ||
          stmt->kind == StmtKind::TraitDecl || stmt->kind == StmtKind::ImplDecl ||
          stmt->kind == StmtKind::Import || stmt->kind == StmtKind::FromImport) {
        continue;
      }
      lowerStmt(*stmt);
    }
    finish();
  }

void Builder::finish() {
    if (terminated_) return;
    Terminator term;
    term.kind = Terminator::Kind::Return;
    fn_.blocks[static_cast<std::size_t>(current_)].term = term;
    terminated_ = true;
  }

int Builder::addBlock(std::string label) {
    Block block;
    block.label = std::move(label);
    fn_.blocks.push_back(std::move(block));
    return static_cast<int>(fn_.blocks.size()) - 1;
  }

std::string Builder::uniqueLabel(const std::string& base) {
    return base + "." + std::to_string(labelCounter_++);
  }

Block& Builder::block(int index) { return fn_.blocks[static_cast<std::size_t>(index)]; }

Inst& Builder::instruction(int id) {
    auto found = defs_.find(id);
    return block(found->second.first).insts[found->second.second];
  }

int Builder::emit(Inst inst) {
    inst.id = fn_.nextValue++;
    defs_[inst.id] = {current_, block(current_).insts.size()};
    if (inst.type) types_[inst.id] = inst.type;
    block(current_).insts.push_back(std::move(inst));
    return fn_.nextValue - 1;
  }

int Builder::emitConstInt(std::int64_t value, TypePtr type) {
    Inst inst;
    inst.op = Op::ConstInt;
    inst.type = std::move(type);
    inst.intValue = value;
    return emit(std::move(inst));
  }

int Builder::emitConst(const Expr& expr) {
    Inst inst;
    inst.span = expr.span;
    switch (expr.kind) {
      case ExprKind::IntLit:
        inst.op = Op::ConstInt;
        inst.intValue = static_cast<const IntLitExpr&>(expr).value;
        break;
      case ExprKind::FloatLit:
        inst.op = Op::ConstFloat;
        inst.floatValue = static_cast<const FloatLitExpr&>(expr).value;
        break;
      case ExprKind::BoolLit:
        inst.op = Op::ConstBool;
        inst.boolValue = static_cast<const BoolLitExpr&>(expr).value;
        break;
      case ExprKind::CharLit:
        inst.op = Op::ConstChar;
        inst.charValue = static_cast<const CharLitExpr&>(expr).value;
        break;
      default:
        inst.op = Op::ConstString;
        break;
    }
    inst.type = defaultType(expr.inferredType);
    return emit(std::move(inst));
  }

int Builder::emitBinary(const std::string& op, int lhs, int rhs, TypePtr type, Span span) {
    Inst inst;
    inst.op = Op::Binary;
    inst.text = op;
    inst.type = std::move(type);
    inst.operands = {lhs, rhs};
    inst.span = span;
    return emit(std::move(inst));
  }

TypePtr Builder::typeOf(int value) const {
    auto found = types_.find(value);
    return found == types_.end() ? nullptr : found->second;
  }

int Builder::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return found->second;
    }
    return -1;
  }

bool Builder::assignVar(const std::string& name, int value) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found == it->end()) continue;
      found->second = value;
      return true;
    }
    return false;
  }

}  // namespace a::ir::detail
