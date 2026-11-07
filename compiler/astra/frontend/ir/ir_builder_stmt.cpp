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

// ---------------------------------------------------------- statements

void Builder::lowerStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let:
        lowerLet(static_cast<const LetStmt&>(stmt));
        return;
      case StmtKind::Assign:
        lowerAssign(static_cast<const AssignStmt&>(stmt));
        return;
      case StmtKind::Expr:
        (void)lowerExpr(*static_cast<const ExprStmt&>(stmt).expr);
        return;
      case StmtKind::Destructure:
        lowerDestructure(static_cast<const DestructureStmt&>(stmt));
        return;
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        int value = ret.value ? lowerExpr(*ret.value) : -1;
        Terminator term;
        term.kind = Terminator::Kind::Return;
        term.value = value;
        term.span = ret.span;
        block(current_).term = term;
        terminated_ = true;
        return;
      }
      case StmtKind::Break: {
        if (loops_.empty()) return;
        jumpTo(loops_.back().exit, snapshotEnv());
        return;
      }
      case StmtKind::Continue: {
        if (loops_.empty()) return;
        jumpTo(loops_.back().continueTarget, snapshotEnv());
        return;
      }
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(stmt);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "defer";
        inst.span = defer.span;
        (void)emit(std::move(inst));
        return;
      }
      case StmtKind::If:
        lowerIf(static_cast<const IfStmt&>(stmt));
        return;
      case StmtKind::While:
        lowerWhile(static_cast<const WhileStmt&>(stmt));
        return;
      case StmtKind::DoWhile:
        lowerDoWhile(static_cast<const DoWhileStmt&>(stmt));
        return;
      case StmtKind::For:
        lowerFor(static_cast<const ForStmt&>(stmt));
        return;
      case StmtKind::Loop:
        lowerLoop(static_cast<const LoopStmt&>(stmt));
        return;
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
    }
  }

void Builder::lowerLet(const LetStmt& let) {
    int value = -1;
    if (let.init) {
      value = lowerExpr(*let.init);
    } else {
      Inst inst;
      inst.op = Op::Opaque;
      inst.text = "uninitialized";
      value = emit(std::move(inst));
    }
    scopes_.back()[let.name] = value;
  }

void Builder::lowerAssign(const AssignStmt& assign) {
    if (assign.op == "=") {
      int value = lowerExpr(*assign.value);
      if (assign.target->kind == ExprKind::Ident) {
        const auto& ident = static_cast<const IdentExpr&>(*assign.target);
        if (!assignVar(ident.name, value)) scopes_.back()[ident.name] = value;
        return;
      }
      storeInto(*assign.target, value, assign.opSpan);
      return;
    }
    int current = lowerExpr(*assign.target);
    int value = lowerExpr(*assign.value);
    int result = emitBinary(assign.op.substr(0, assign.op.size() - 1), current, value,
                            defaultType(assign.target->inferredType), assign.opSpan);
    if (assign.target->kind == ExprKind::Ident) {
      assignVar(static_cast<const IdentExpr&>(*assign.target).name, result);
    } else {
      storeInto(*assign.target, result, assign.opSpan);
    }
  }

void Builder::storeInto(const Expr& target, int value, Span span) {
    Inst inst;
    inst.span = span;
    if (target.kind == ExprKind::Index) {
      const auto& index = static_cast<const IndexExpr&>(target);
      inst.op = Op::StoreIndex;
      inst.operands = {lowerExpr(*index.object), lowerExpr(*index.index), value};
    } else if (target.kind == ExprKind::Field) {
      const auto& field = static_cast<const FieldExpr&>(target);
      inst.op = Op::StoreField;
      inst.text = field.field;
      inst.operands = {lowerExpr(*field.object), value};
    } else {
      inst.op = Op::Opaque;
      inst.text = "store";
      inst.operands = {value};
    }
    (void)emit(std::move(inst));
  }

void Builder::lowerDestructure(const DestructureStmt& stmt) {
    int value = lowerExpr(*stmt.value);
    for (std::size_t i = 0; i < stmt.names.size(); ++i) {
      Inst inst;
      inst.op = Op::TupleGet;
      inst.intValue = static_cast<std::int64_t>(i);
      inst.operands = {value};
      int element = emit(std::move(inst));
      if (stmt.names[i] != "_") scopes_.back()[stmt.names[i]] = element;
    }
  }

void Builder::lowerIf(const IfStmt& stmt) {
    auto saved = scopes_;
    int merge = addBlock(uniqueLabel("if.end"));
    int condBlock = current_;
    bool hasElse = !stmt.elseBody.empty();
    for (std::size_t i = 0; i < stmt.branches.size(); ++i) {
      current_ = condBlock;
      terminated_ = false;
      scopes_ = saved;
      int condition = lowerExpr(*stmt.branches[i].cond);
      int body = addBlock(uniqueLabel("if.then"));
      int next = i + 1 < stmt.branches.size()
                     ? addBlock(uniqueLabel("if.cond"))
                     : (hasElse ? addBlock(uniqueLabel("if.else")) : merge);
      branchTo(condition, body, next, snapshotEnv());

      current_ = body;
      terminated_ = false;
      scopes_ = saved;
      scopes_.emplace_back();
      lowerStatementsRaw(stmt.branches[i].body);
      scopes_.pop_back();
      if (!terminated_) jumpTo(merge, snapshotEnv());
      condBlock = next;
    }
    if (hasElse) {
      current_ = condBlock;
      terminated_ = false;
      scopes_ = saved;
      scopes_.emplace_back();
      lowerStatementsRaw(stmt.elseBody);
      scopes_.pop_back();
      if (!terminated_) jumpTo(merge, snapshotEnv());
    }
    scopes_ = saved;
    current_ = merge;
    terminated_ = false;
    materializeJoin(merge);
    if (blockIncoming_[merge].empty()) {
      block(merge).term = Terminator{};
      terminated_ = true;
    }
  }

// Shared prologue for loops: create the header phis and return the
// environment variables should see at the header.
StringMap<int> Builder::createHeaderPhis(int header, int preheader,
                                  const std::vector<std::string>& names) {
    StringMap<int> headerEnv;
    const Edge& entry = blockIncoming_[header].front();
    for (const auto& name : names) {
      int initial = -1;
      if (!lookupEnv(entry.env, name, initial)) continue;
      int phi = pushPhi(header, defaultType(typeOf(initial)), initial, preheader);
      headerEnv[name] = phi;
    }
    return headerEnv;
  }

std::vector<std::string> Builder::assignedNames(const std::vector<StmtPtr>& body) {
    std::set<std::string> names;
    collectAssigned(body, names);
    std::vector<std::string> out;
    for (const auto& name : names) {
      if (lookup(name) >= 0) out.push_back(name);
    }
    return out;
  }

void Builder::lowerWhile(const WhileStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int cond = addBlock(uniqueLabel("while.cond"));
    int body = addBlock(uniqueLabel("while.body"));
    int exit = addBlock(uniqueLabel("while.exit"));
    jumpTo(cond, snapshotEnv());

    LoopContext loop;
    loop.header = cond;
    loop.exit = exit;
    loop.continueTarget = cond;
    StringMap<int> headerEnv = createHeaderPhis(cond, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = cond;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    int condition = lowerExpr(*stmt.cond);
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(cond, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

void Builder::lowerDoWhile(const DoWhileStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int body = addBlock(uniqueLabel("do.body"));
    int cond = addBlock(uniqueLabel("do.cond"));
    int exit = addBlock(uniqueLabel("do.exit"));
    jumpTo(body, snapshotEnv());

    LoopContext loop;
    loop.header = body;
    loop.exit = exit;
    loop.continueTarget = cond;
    StringMap<int> headerEnv = createHeaderPhis(body, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(cond, snapshotEnv());

    current_ = cond;
    terminated_ = false;
    scopes_ = saved;
    materializeJoin(cond);
    int condition = lowerExpr(*stmt.cond);
    branchTo(condition, body, exit, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

void Builder::lowerLoop(const LoopStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int header = addBlock(uniqueLabel("loop.header"));
    int body = addBlock(uniqueLabel("loop.body"));
    int exit = addBlock(uniqueLabel("loop.exit"));
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = header;
    StringMap<int> headerEnv = createHeaderPhis(header, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    jumpTo(body, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(header, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

void Builder::lowerFor(const ForStmt& stmt) {
    if (stmt.iterable->kind == ExprKind::Range) {
      lowerForRange(stmt, static_cast<const RangeExpr&>(*stmt.iterable));
      return;
    }
    lowerForIterable(stmt);
  }

void Builder::lowerForRange(const ForStmt& stmt, const RangeExpr& range) {
    auto saved = scopes_;
    int start = lowerExpr(*range.start);
    int end = lowerExpr(*range.end);
    int step = range.step ? lowerExpr(*range.step) : emitConstInt(1);

    int header = addBlock(uniqueLabel("for.cond"));
    int body = addBlock(uniqueLabel("for.body"));
    int latch = addBlock(uniqueLabel("for.latch"));
    int exit = addBlock(uniqueLabel("for.exit"));
    int preheader = current_;
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = latch;
    int index = pushPhi(header, intType(), start, preheader);
    loop.headerPhis.push_back({"", index});
    StringMap<int> headerEnv = createHeaderPhis(header, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    int condition = emitBinary("<", index, end, typeBool(), range.span);
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    scopes_.back()[stmt.var] = index;
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(latch, snapshotEnv());

    current_ = latch;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    materializeJoin(latch);
    int next = emitBinary("+", index, step, intType(), range.span);
    jumpTo(header, snapshotEnv(), next);
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

TypePtr Builder::iterableElement(const TypePtr& type) {
    TypePtr resolved = defaultType(type);
    if (!resolved) return nullptr;
    switch (resolved->kind) {
      case TypeKind::Array:
      case TypeKind::Set:
      case TypeKind::Range:
        return resolved->elem;
      case TypeKind::Map:
        return resolved->elem;
      case TypeKind::String:
        return typeChar();
      default:
        return nullptr;
    }
  }

void Builder::lowerForIterable(const ForStmt& stmt) {
    auto saved = scopes_;
    int iterable = lowerExpr(*stmt.iterable);
    Inst init;
    init.op = Op::IterInit;
    init.type = stmt.iterable->inferredType;
    init.operands = {iterable};
    int iterator = emit(std::move(init));

    int header = addBlock(uniqueLabel("for.cond"));
    int body = addBlock(uniqueLabel("for.body"));
    int exit = addBlock(uniqueLabel("for.exit"));
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = header;
    StringMap<int> headerEnv = createHeaderPhis(header, header, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    Inst has;
    has.op = Op::IterHasNext;
    has.type = typeBool();
    has.operands = {iterator};
    int condition = emit(std::move(has));
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    Inst next;
    next.op = Op::IterNext;
    next.type = iterableElement(stmt.iterable->inferredType);
    next.operands = {iterator};
    scopes_.back()[stmt.var] = emit(std::move(next));
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(header, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

}  // namespace a::ir::detail
