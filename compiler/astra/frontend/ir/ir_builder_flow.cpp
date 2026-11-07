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

StringMap<int> Builder::snapshotEnv() const {
    StringMap<int> env;
    for (const auto& scope : scopes_) {
      for (const auto& entry : scope) env[entry.first] = entry.second;
    }
    return env;
  }

void Builder::applyEnv(const StringMap<int>& env) {
    for (const auto& entry : env) assignVar(entry.first, entry.second);
  }

void Builder::lowerStatements(const std::vector<StmtPtr>& body) {
    scopes_.emplace_back();
    for (const auto& stmt : body) {
      if (terminated_) break;
      lowerStmt(*stmt);
    }
    scopes_.pop_back();
  }

void Builder::lowerStatementsRaw(const std::vector<StmtPtr>& body) {
    for (const auto& stmt : body) {
      if (terminated_) break;
      lowerStmt(*stmt);
    }
  }

// ------------------------------------------------------- terminators

void Builder::recordEdge(int target, const StringMap<int>& env, int extra) {
    blockIncoming_[target].push_back(Edge{current_, env});
    for (auto& loop : loops_) {
      if (loop.header != target) continue;
      for (const auto& phi : loop.headerPhis) {
        int value = -1;
        if (phi.first.empty()) {
          value = extra;
        } else {
          lookupEnv(env, phi.first, value);
        }
        if (value < 0) continue;
        Inst& inst = instruction(phi.second);
        inst.operands.push_back(value);
        inst.incomingBlocks.push_back(current_);
      }
    }
  }

void Builder::jumpTo(int target, const StringMap<int>& env, int extra) {
    recordEdge(target, env, extra);
    Terminator term;
    term.kind = Terminator::Kind::Jump;
    term.target = target;
    block(current_).term = term;
    terminated_ = true;
  }

void Builder::branchTo(int condition, int trueTarget, int falseTarget, const StringMap<int>& env) {
    recordEdge(trueTarget, env);
    recordEdge(falseTarget, env);
    Terminator term;
    term.kind = Terminator::Kind::Branch;
    term.target = trueTarget;
    term.falseTarget = falseTarget;
    term.value = condition;
    block(current_).term = term;
    terminated_ = true;
  }

int Builder::pushPhi(int blockIndex, TypePtr type, int value, int from) {
    Inst inst;
    inst.op = Op::Phi;
    inst.type = std::move(type);
    inst.operands.push_back(value);
    inst.incomingBlocks.push_back(from);
    inst.id = fn_.nextValue++;
    defs_[inst.id] = {blockIndex, block(blockIndex).insts.size()};
    if (inst.type) types_[inst.id] = inst.type;
    block(blockIndex).insts.push_back(std::move(inst));
    return fn_.nextValue - 1;
  }

// Creates the phis a join block needs given every incoming edge. Variables
// that keep the same value on every edge are bound directly, without a phi.
std::size_t Builder::materializeJoin(int blockIndex) {
    auto found = blockIncoming_.find(blockIndex);
    if (found == blockIncoming_.end() || found->second.empty()) return 0;
    const std::vector<Edge>& edges = found->second;
    std::size_t created = 0;
    if (edges.size() == 1) {
      applyEnv(edges.front().env);
      return 0;
    }
    std::vector<std::string> names;
    for (const auto& entry : edges.front().env) {
      bool present = true;
      for (std::size_t i = 1; i < edges.size(); ++i) {
        if (!edges[i].env.count(entry.first)) {
          present = false;
          break;
        }
      }
      if (present) names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      std::vector<int> values;
      values.reserve(edges.size());
      bool same = true;
      for (const auto& edge : edges) {
        int value = edge.env.at(name);
        if (!values.empty() && values.front() != value) same = false;
        values.push_back(value);
      }
      if (same) {
        assignVar(name, values.front());
        continue;
      }
      Inst phi;
      phi.op = Op::Phi;
      phi.type = defaultType(typeOf(values.front()));
      phi.id = fn_.nextValue++;
      defs_[phi.id] = {blockIndex, block(blockIndex).insts.size()};
      if (phi.type) types_[phi.id] = phi.type;
      for (std::size_t i = 0; i < edges.size(); ++i) {
        phi.operands.push_back(values[i]);
        phi.incomingBlocks.push_back(edges[i].from);
      }
      block(blockIndex).insts.push_back(std::move(phi));
      assignVar(name, fn_.nextValue - 1);
      ++created;
    }
    return created;
  }

}  // namespace a::ir::detail
