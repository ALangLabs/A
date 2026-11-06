#include "backend/kgen/codegen/llvm_emitter.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <set>
#include <filesystem>
#include <cstdlib>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>

#include "name-resolution/native_modules.hpp"
#include "backend/kgen/native_backend.hpp"

namespace a {
namespace {

bool isStdlibModule(const ModuleGraph& graph, const Module& mod);
bool nativeFloatScanOk(const ModuleGraph& graph);
bool exprProvablyFloat(const Expr& expr);

std::string extractString(const StringLitExpr& s) {
  std::string result;
  for (const auto& p : s.parts) {
    if (!p.isExpr) {
      result += p.text;
    }
  }
  return result;
}

// ============================================================================
// LLVM IR Textual Emitter
// ============================================================================
class LLVMEmitter {
 public:
  LLVMEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    emitTargetHeader();
    emitDeclarations();

    // Collect global string literals
    collectStringLiterals();
    emitStringConstants();

    // Emit user function definitions (stdlib/prelude modules are skipped:
    // they are not part of the native subset and are never called natively)
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program || isStdlibModule(graph_,mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit main entrypoint
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int tempCounter_ = 1;
  int labelCounter_ = 1;
  int strCounter_ = 1;
  std::unordered_map<std::string, std::string> locals_;
  std::unordered_map<std::string, std::string> strTable_;

  std::string nextTemp() {
    return "%t" + std::to_string(tempCounter_++);
  }

  std::string nextLabel(const std::string& prefix = "bb") {
    return prefix + std::to_string(labelCounter_++);
  }

  void collectStringLiterals() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModule(graph_,mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        collectFromStmt(*stmt);
      }
    }
  }

  void collectFromStmt(const Stmt& stmt) {
    if (stmt.kind == StmtKind::Expr) {
      collectFromExpr(*static_cast<const ExprStmt&>(stmt).expr);
    } else if (stmt.kind == StmtKind::Let) {
      const auto& let = static_cast<const LetStmt&>(stmt);
      if (let.init) collectFromExpr(*let.init);
    } else if (stmt.kind == StmtKind::Assign) {
      const auto& a = static_cast<const AssignStmt&>(stmt);
      if (a.value) collectFromExpr(*a.value);
    } else if (stmt.kind == StmtKind::FnDecl) {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      for (const auto& s : fn.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::If) {
      const auto& ifs = static_cast<const IfStmt&>(stmt);
      for (const auto& br : ifs.branches) {
        if (br.cond) collectFromExpr(*br.cond);
        for (const auto& s : br.body) collectFromStmt(*s);
      }
      for (const auto& s : ifs.elseBody) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::While) {
      const auto& wh = static_cast<const WhileStmt&>(stmt);
      if (wh.cond) collectFromExpr(*wh.cond);
      for (const auto& s : wh.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::For) {
      const auto& fo = static_cast<const ForStmt&>(stmt);
      if (fo.iterable) collectFromExpr(*fo.iterable);
      for (const auto& s : fo.body) collectFromStmt(*s);
    }
  }

  void collectFromExpr(const Expr& expr) {
    if (expr.kind == ExprKind::StringLit) {
      std::string s = extractString(static_cast<const StringLitExpr&>(expr));
      if (strTable_.find(s) == strTable_.end()) {
        std::string name = "@.str." + std::to_string(strCounter_++);
        strTable_[s] = name;
      }
    } else if (expr.kind == ExprKind::Binary) {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      collectFromExpr(*b.lhs);
      collectFromExpr(*b.rhs);
    } else if (expr.kind == ExprKind::Call) {
      const auto& c = static_cast<const CallExpr&>(expr);
      collectFromExpr(*c.callee);
      for (const auto& a : c.args) collectFromExpr(*a.value);
    }
  }

  void emitTargetHeader() {
    out_ << "; ModuleID = 'a_program'\n";
    out_ << "source_filename = \"a_program.a\"\n";
    out_ << "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n";
    out_ << "target triple = \"x86_64-pc-linux-gnu\"\n\n";

    // Format strings for standard runtime printing
    out_ << "@.fmt_int = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\", align 1\n";
    out_ << "@.fmt_float = private unnamed_addr constant [6 x i8] c\"%.2f\\0A\\00\", align 1\n";
    out_ << "@.fmt_str = private unnamed_addr constant [4 x i8] c\"%s\\0A\\00\", align 1\n\n";
  }

  void emitStringConstants() {
    for (const auto& pair : strTable_) {
      const std::string& str = pair.first;
      const std::string& name = pair.second;
      std::size_t len = str.size() + 1;
      out_ << name << " = private unnamed_addr constant [" << len << " x i8] c\"";
      for (char c : str) {
        if (c == '\n') out_ << "\\0A";
        else if (c == '\t') out_ << "\\09";
        else if (c == '\"') out_ << "\\22";
        else if (c == '\\') out_ << "\\5C";
        else out_ << c;
      }
      out_ << "\\00\", align 1\n";
    }
    if (!strTable_.empty()) out_ << "\n";
  }

  void emitDeclarations() {
    out_ << "declare i32 @printf(ptr, ...)\n";
    out_ << "declare double @sqrt(double)\n";
    out_ << "declare double @round(double)\n";
    out_ << "declare double @pow(double, double)\n\n";
  }

  std::string mangle(const std::string& name) {
    if (name == "main") return "_a_main";
    return "_a_" + name;
  }

  void emitFunction(const FnDecl& fn) {
    locals_.clear();
    tempCounter_ = 1;
    labelCounter_ = 1;

    out_ << "define i64 @" << mangle(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i > 0) out_ << ", ";
      out_ << "i64 %arg_" << fn.params[i].name;
    }
    out_ << ") {\n";
    out_ << "entry:\n";

    // Stack space for parameters
    for (const auto& param : fn.params) {
      std::string ptr = nextTemp();
      out_ << "  " << ptr << " = alloca i64, align 8\n";
      out_ << "  store i64 %arg_" << param.name << ", ptr " << ptr << ", align 8\n";
      locals_[param.name] = ptr;
    }

    for (const auto& stmt : fn.body) {
      emitStmt(*stmt);
    }

    out_ << "  ret i64 0\n";
    out_ << "}\n\n";
  }

  void emitStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        std::string ptr = nextTemp();
        out_ << "  " << ptr << " = alloca i64, align 8\n";
        locals_[let.name] = ptr;
        if (let.init) {
          std::string val = emitExpr(*let.init);
          out_ << "  store i64 " << val << ", ptr " << ptr << ", align 8\n";
        }
        break;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        if (assign.target->kind == ExprKind::Ident) {
          const auto& id = static_cast<const IdentExpr&>(*assign.target);
          if (locals_.find(id.name) == locals_.end()) {
            std::string ptr = nextTemp();
            out_ << "  " << ptr << " = alloca i64, align 8\n";
            locals_[id.name] = ptr;
          }
          std::string val = emitExpr(*assign.value);
          if (assign.op == "=") {
            out_ << "  store i64 " << val << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "+=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string sum = nextTemp();
            out_ << "  " << sum << " = add nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << sum << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "-=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string diff = nextTemp();
            out_ << "  " << diff << " = sub nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << diff << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "*=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string prod = nextTemp();
            out_ << "  " << prod << " = mul nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << prod << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "/=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string quot = nextTemp();
            out_ << "  " << quot << " = sdiv i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << quot << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "%=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string rem = nextTemp();
            out_ << "  " << rem << " = srem i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << rem << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "&=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = and i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "|=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = or i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "^=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = xor i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "<<=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = shl i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == ">>=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = ashr i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          }
        }
        break;
      }
      case StmtKind::If: {
        const auto& ifStmt = static_cast<const IfStmt&>(stmt);
        if (ifStmt.branches.empty()) break;
        std::string endLbl = nextLabel("if.end");

        for (std::size_t i = 0; i < ifStmt.branches.size(); ++i) {
          const auto& br = ifStmt.branches[i];
          std::string condVal = emitExpr(*br.cond);
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp ne i64 " << condVal << ", 0\n";

          std::string thenLbl = nextLabel("if.then");
          std::string nextArmLbl = (i + 1 < ifStmt.branches.size())
                                       ? nextLabel("if.branch")
                                       : (!ifStmt.elseBody.empty() ? nextLabel("if.else") : endLbl);

          out_ << "  br i1 " << cmp << ", label %" << thenLbl << ", label %" << nextArmLbl << "\n\n";

          out_ << thenLbl << ":\n";
          for (const auto& s : br.body) emitStmt(*s);
          out_ << "  br label %" << endLbl << "\n\n";

          if (i + 1 < ifStmt.branches.size() || !ifStmt.elseBody.empty()) {
            out_ << nextArmLbl << ":\n";
          }
        }

        if (!ifStmt.elseBody.empty()) {
          for (const auto& s : ifStmt.elseBody) emitStmt(*s);
          out_ << "  br label %" << endLbl << "\n\n";
        }

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::While: {
        const auto& whileStmt = static_cast<const WhileStmt&>(stmt);
        std::string condLbl = nextLabel("while.cond");
        std::string bodyLbl = nextLabel("while.body");
        std::string endLbl = nextLabel("while.end");

        out_ << "  br label %" << condLbl << "\n\n";
        out_ << condLbl << ":\n";
        std::string condVal = emitExpr(*whileStmt.cond);
        std::string cmp = nextTemp();
        out_ << "  " << cmp << " = icmp ne i64 " << condVal << ", 0\n";
        out_ << "  br i1 " << cmp << ", label %" << bodyLbl << ", label %" << endLbl << "\n\n";

        out_ << bodyLbl << ":\n";
        for (const auto& s : whileStmt.body) emitStmt(*s);
        out_ << "  br label %" << condLbl << "\n\n";

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::For: {
        const auto& forStmt = static_cast<const ForStmt&>(stmt);
        if (forStmt.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*forStmt.iterable);
          std::string startVal = emitExpr(*range.start);
          std::string endVal = emitExpr(*range.end);
          
          std::string varPtr = nextTemp();
          out_ << "  " << varPtr << " = alloca i64, align 8\n";
          out_ << "  store i64 " << startVal << ", ptr " << varPtr << ", align 8\n";
          locals_[forStmt.var] = varPtr;

          std::string condLabel = nextLabel("for.cond");
          std::string bodyLabel = nextLabel("for.body");
          std::string incLabel = nextLabel("for.inc");
          std::string endLabel = nextLabel("for.end");

          out_ << "  br label %" << condLabel << "\n\n";

          out_ << condLabel << ":\n";
          std::string curVal = nextTemp();
          out_ << "  " << curVal << " = load i64, ptr " << varPtr << ", align 8\n";
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp slt i64 " << curVal << ", " << endVal << "\n";
          out_ << "  br i1 " << cmp << ", label %" << bodyLabel << ", label %" << endLabel << "\n\n";

          out_ << bodyLabel << ":\n";
          for (const auto& s : forStmt.body) emitStmt(*s);
          out_ << "  br label %" << incLabel << "\n\n";

          out_ << incLabel << ":\n";
          std::string cur2 = nextTemp();
          out_ << "  " << cur2 << " = load i64, ptr " << varPtr << ", align 8\n";
          std::string next = nextTemp();
          out_ << "  " << next << " = add nsw i64 " << cur2 << ", 1\n";
          out_ << "  store i64 " << next << ", ptr " << varPtr << ", align 8\n";
          out_ << "  br label %" << condLabel << "\n\n";

          out_ << endLabel << ":\n";
        }
        break;
      }
      case StmtKind::Expr: {
        const auto& exprStmt = static_cast<const ExprStmt&>(stmt);
        emitExpr(*exprStmt.expr);
        break;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
          std::string v = emitExpr(*ret.value);
          out_ << "  ret i64 " << v << "\n";
        } else {
          out_ << "  ret i64 0\n";
        }
        break;
      }
      default:
        break;
    }
  }

  std::string emitExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
        return std::to_string(static_cast<const IntLitExpr&>(expr).value);
      case ExprKind::BoolLit:
        return static_cast<const BoolLitExpr&>(expr).value ? "1" : "0";
      case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(expr);
        auto it = locals_.find(id.name);
        if (it != locals_.end()) {
          std::string val = nextTemp();
          out_ << "  " << val << " = load i64, ptr " << it->second << ", align 8\n";
          return val;
        }
        return "0";
      }
      case ExprKind::Binary: {
        const auto& bin = static_cast<const BinaryExpr&>(expr);
        std::string l = emitExpr(*bin.lhs);
        std::string r = emitExpr(*bin.rhs);
        std::string res = nextTemp();
        if (bin.op == "+") {
          out_ << "  " << res << " = add nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "-") {
          out_ << "  " << res << " = sub nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "*") {
          out_ << "  " << res << " = mul nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "/") {
          out_ << "  " << res << " = sdiv i64 " << l << ", " << r << "\n";
        } else if (bin.op == "%") {
          out_ << "  " << res << " = srem i64 " << l << ", " << r << "\n";
        } else if (bin.op == "==") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp eq i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "!=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp ne i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "<") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp slt i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "<=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sle i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == ">") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sgt i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == ">=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sge i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "and" || bin.op == "&") {
          out_ << "  " << res << " = and i64 " << l << ", " << r << "\n";
        } else if (bin.op == "or" || bin.op == "|") {
          out_ << "  " << res << " = or i64 " << l << ", " << r << "\n";
        } else if (bin.op == "^") {
          out_ << "  " << res << " = xor i64 " << l << ", " << r << "\n";
        } else if (bin.op == "<<") {
          out_ << "  " << res << " = shl i64 " << l << ", " << r << "\n";
        } else if (bin.op == ">>") {
          out_ << "  " << res << " = ashr i64 " << l << ", " << r << "\n";
        } else {
          out_ << "  " << res << " = add i64 " << l << ", " << r << "\n";
        }
        return res;
      }
      case ExprKind::Unary: {
        const auto& un = static_cast<const UnaryExpr&>(expr);
        std::string operand = emitExpr(*un.operand);
        std::string res = nextTemp();
        if (un.op == "-") {
          out_ << "  " << res << " = sub nsw i64 0, " << operand << "\n";
        } else if (un.op == "not" || un.op == "!") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp eq i64 " << operand << ", 0\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (un.op == "~") {
          out_ << "  " << res << " = xor i64 " << operand << ", -1\n";
        }
        return res;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*call.callee).name;
          if (name == "print") {
            if (!call.args.empty()) {
              const auto& argExpr = *call.args[0].value;
              if (argExpr.kind == ExprKind::StringLit) {
                std::string s = extractString(static_cast<const StringLitExpr&>(argExpr));
                std::string strSym = strTable_[s];
                std::string callRes = nextTemp();
                out_ << "  " << callRes << " = call i32 (ptr, ...) @printf(ptr @.fmt_str, ptr " << strSym << ")\n";
                return "0";
              }
              std::string arg = emitExpr(argExpr);
              std::string callRes = nextTemp();
              out_ << "  " << callRes << " = call i32 (ptr, ...) @printf(ptr @.fmt_int, i64 " << arg << ")\n";
              return "0";
            }
          }
          // General user function call
          std::vector<std::string> args;
          for (const auto& a : call.args) args.push_back(emitExpr(*a.value));
          std::string callRes = nextTemp();
          out_ << "  " << callRes << " = call i64 @" << mangle(name) << "(";
          for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) out_ << ", ";
            out_ << "i64 " << args[i];
          }
          out_ << ")\n";
          return callRes;
        }
        return "0";
      }
      default:
        return "0";
    }
  }

  void emitMain() {
    out_ << "define i32 @main(i32 %argc, ptr %argv) {\n";
    out_ << "entry:\n";
    if (graph_.entry >= 0 && static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) {
            emitStmt(*stmt);
          }
        }
      }
    }
    out_ << "  ret i32 0\n";
    out_ << "}\n";
  }
};

// ============================================================================
// Direct GNU x86-64 Assembly Emitter
// ============================================================================
class X86Emitter {
 public:
  X86Emitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  void collectGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      std::string name;
      if (stmt->kind == StmtKind::Let) {
        name = static_cast<const LetStmt&>(*stmt).name;
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        name = static_cast<const IdentExpr&>(*static_cast<const AssignStmt&>(*stmt).target).name;
      }
      if (!name.empty() && !globalSlots_.count(name)) {
        globalSlots_[name] = globalBytes_;
        globalBytes_ += 8;
      }
    }
  }

  // Type the globals before functions are emitted (they are compiled above
  // main, so without this a function reading a float global would read it as
  // an int). Walks top-level statements in order.
  void pretypeGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      if (stmt->kind == StmtKind::Let) {
        const auto& s = static_cast<const LetStmt&>(*stmt);
        if (s.init && globalSlots_.count(s.name))
          globalIsFloat_[s.name] = exprIsFloat(*s.init);
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        const auto& s = static_cast<const AssignStmt&>(*stmt);
        const auto& id = static_cast<const IdentExpr&>(*s.target);
        if (!globalSlots_.count(id.name)) continue;
        bool vf = exprIsFloat(*s.value);
        if (s.op == "=") globalIsFloat_[id.name] = vf;
        else globalIsFloat_[id.name] = vf || globalIsFloat_[id.name];
      }
    }
  }

  bool emit() {
    collectStringLiterals();
    collectGlobals();
    pretypeGlobals();

    out_ << ".intel_syntax noprefix\n";
    out_ << ".text\n\n";

    // Format strings
    out_ << ".section .rodata\n";
    out_ << ".Lfmt_int:\n  .asciz \"%lld\\n\"\n";
    out_ << ".Lfmt_float:\n  .asciz \"%.17g\\n\"\n";
    out_ << ".Lfmt_str:\n  .asciz \"%s\\n\"\n";

    for (const auto& pair : floatTable_) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", pair.first);
      out_ << pair.second << ":\n  .double " << buf << "\n";
    }
    for (const auto& pair : strTable_) {
      out_ << pair.second << ":\n  .asciz \"";
      for (char c : pair.first) {
        if (c == '\n') out_ << "\\n";
        else if (c == '\t') out_ << "\\t";
        else if (c == '\"') out_ << "\\\"";
        else if (c == '\\') out_ << "\\\\";
        else out_ << c;
      }
      out_ << "\"\n";
    }

    out_ << "\n.text\n";
    if (globalBytes_ > 0) {
      out_ << ".comm _a_globals, " << globalBytes_ << ", 32\n\n";
    }

    // Emit functions (stdlib/prelude modules skipped, same as LLVMEmitter)
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program || isStdlibModule(graph_,mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit main
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int labelCounter_ = 1;
  int strCounter_ = 1;
  int currentStackOffset_ = 0;
  bool inMain_ = false;
  std::unordered_map<std::string, int> stackSlots_;
  std::unordered_map<std::string, std::string> strTable_;
  // Module-level bindings live in a dedicated .bss area so functions can read
  // them (Goose-style flat global storage, one 8-byte slot per name).
  std::unordered_map<std::string, int> globalSlots_;
  int globalBytes_ = 0;
  // Value typing for the native subset: slots are 8 bytes and may hold an
  // i64 or an IEEE-754 double bit pattern. "f64" entries are doubles.
  std::unordered_map<std::string, bool> slotIsFloat_;   // function/top-level locals
  std::unordered_map<std::string, bool> globalIsFloat_; // module globals
  std::unordered_map<double, std::string> floatTable_;
  int floatCounter_ = 1;

  std::string nextLabel(const std::string& prefix = "L") {
    return "." + prefix + "_" + std::to_string(labelCounter_++);
  }

  void collectStringLiterals() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModule(graph_,mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        collectFromStmt(*stmt);
      }
    }
  }

  void collectFromStmt(const Stmt& stmt) {
    if (stmt.kind == StmtKind::Expr) {
      collectFromExpr(*static_cast<const ExprStmt&>(stmt).expr);
    } else if (stmt.kind == StmtKind::Let) {
      const auto& let = static_cast<const LetStmt&>(stmt);
      if (let.init) collectFromExpr(*let.init);
    } else if (stmt.kind == StmtKind::Assign) {
      const auto& a = static_cast<const AssignStmt&>(stmt);
      if (a.value) collectFromExpr(*a.value);
    } else if (stmt.kind == StmtKind::FnDecl) {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      for (const auto& s : fn.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::If) {
      const auto& ifs = static_cast<const IfStmt&>(stmt);
      for (const auto& br : ifs.branches) {
        if (br.cond) collectFromExpr(*br.cond);
        for (const auto& s : br.body) collectFromStmt(*s);
      }
      for (const auto& s : ifs.elseBody) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::While) {
      const auto& wh = static_cast<const WhileStmt&>(stmt);
      if (wh.cond) collectFromExpr(*wh.cond);
      for (const auto& s : wh.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::For) {
      const auto& fo = static_cast<const ForStmt&>(stmt);
      if (fo.iterable) collectFromExpr(*fo.iterable);
      for (const auto& s : fo.body) collectFromStmt(*s);
    }
  }

  void collectFromExpr(const Expr& expr) {
    if (expr.kind == ExprKind::StringLit) {
      std::string s = extractString(static_cast<const StringLitExpr&>(expr));
      if (strTable_.find(s) == strTable_.end()) {
        strTable_[s] = ".Lstr_" + std::to_string(strCounter_++);
      }
    } else if (expr.kind == ExprKind::FloatLit) {
      double v = static_cast<const FloatLitExpr&>(expr).value;
      if (floatTable_.find(v) == floatTable_.end()) {
        floatTable_[v] = ".LCf_" + std::to_string(floatCounter_++);
      }
    } else if (expr.kind == ExprKind::Call) {
      const auto& c = static_cast<const CallExpr&>(expr);
      for (const auto& a : c.args) collectFromExpr(*a.value);
    } else if (expr.kind == ExprKind::Binary) {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      collectFromExpr(*b.lhs); collectFromExpr(*b.rhs);
    } else if (expr.kind == ExprKind::Unary) {
      collectFromExpr(*static_cast<const UnaryExpr&>(expr).operand);
    } else if (expr.kind == ExprKind::Range) {
      const auto& r = static_cast<const RangeExpr&>(expr);
      collectFromExpr(*r.start); collectFromExpr(*r.end);
      if (r.step) collectFromExpr(*r.step);
    }
  }

  // Does this expression evaluate to a double in the native subset?
  bool exprIsFloat(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::FloatLit: return true;
      case ExprKind::Ident: {
        const std::string& n = static_cast<const IdentExpr&>(expr).name;
        auto li = slotIsFloat_.find(n);
        if (li != slotIsFloat_.end()) return li->second;
        auto gi = globalIsFloat_.find(n);
        return gi != globalIsFloat_.end() && gi->second;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(expr);
        static const std::set<std::string> arith = {"+","-","*","/","%"};
        return arith.count(b.op) && (exprIsFloat(*b.lhs) || exprIsFloat(*b.rhs));
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(expr);
        return u.op == "-" && exprIsFloat(*u.operand);
      }
      case ExprKind::Call:
        return exprIsFloatCall(static_cast<const CallExpr&>(expr).callee.get());
      default: return false;
    }
  }
  bool exprIsFloatCall(const Expr* callee) {
    if (!callee || callee->kind != ExprKind::Ident) return false;
    static const std::set<std::string> fns = {
        "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos","tan",
        "fabs","f64","float"};
    return fns.count(static_cast<const IdentExpr&>(*callee).name) != 0;
  }

  int allocateSlot(const std::string& name) {
    auto it = stackSlots_.find(name);
    if (it != stackSlots_.end()) return it->second;
    currentStackOffset_ += 8;
    stackSlots_[name] = currentStackOffset_;
    return currentStackOffset_;
  }

  void emitFunction(const FnDecl& fn) {
    stackSlots_.clear();
    slotIsFloat_.clear();
    currentStackOffset_ = 0;

    std::string fnLabel = fn.name == "main" ? "_a_main" : "_a_" + fn.name;
    out_ << ".globl " << fnLabel << "\n";
    out_ << ".type " << fnLabel << ", @function\n";
    out_ << fnLabel << ":\n";
    out_ << "  push rbp\n";
    out_ << "  mov rbp, rsp\n";
    out_ << "  sub rsp, 256\n";  // Reserve stack frame

    static const char* kArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (std::size_t i = 0; i < fn.params.size() && i < 6; ++i) {
      int slot = allocateSlot(fn.params[i].name);
      out_ << "  mov [rbp - " << slot << "], " << kArgRegs[i] << "\n";
    }

    for (const auto& s : fn.body) emitStmt(*s);

    out_ << "  mov rax, 0\n";
    out_ << "  mov rsp, rbp\n";
    out_ << "  pop rbp\n";
    out_ << "  ret\n\n";
  }

  void emitStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        auto git = globalSlots_.find(let.name);
        bool toGlobal = inMain_ && git != globalSlots_.end();
        int slot = toGlobal ? -1 : allocateSlot(let.name);
        if (let.init) {
          bool fv = exprIsFloat(*let.init);
          if (fv) emitExpr(*let.init); else emitIOperand(*let.init);
          slotIsFloat_[let.name] = fv;   // Let is always a local binding
          if (toGlobal) {
            out_ << "  lea r10, [rip+_a_globals]\n";
            if (fv) out_ << "  movsd [r10+" << git->second << "], xmm0\n";
            else out_ << "  mov [r10+" << git->second << "], rax\n";
          } else {
            if (fv) out_ << "  movsd [rbp - " << slot << "], xmm0\n";
            else out_ << "  mov [rbp - " << slot << "], rax\n";
          }
        }
        break;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        if (assign.target->kind == ExprKind::Ident) {
          const auto& id = static_cast<const IdentExpr&>(*assign.target);
          auto git = globalSlots_.find(id.name);
          // Assign resolves through the env chain: a name bound at module
          // level is mutated, not shadowed (matches the interpreter).
          bool toGlobal = git != globalSlots_.end() &&
                          stackSlots_.find(id.name) == stackSlots_.end();
          int slot = toGlobal ? -1 : allocateSlot(id.name);
          bool valFloat = exprIsFloat(*assign.value);
          bool curFloat = (toGlobal ? globalIsFloat_ : slotIsFloat_)[id.name];
          if (assign.op == "=") {
            if (valFloat) emitExpr(*assign.value); else emitIOperand(*assign.value);
            (toGlobal ? globalIsFloat_ : slotIsFloat_)[id.name] = valFloat;
            if (toGlobal) {
              out_ << "  lea r10, [rip+_a_globals]\n";
              if (valFloat) out_ << "  movsd [r10+" << git->second << "], xmm0\n";
              else out_ << "  mov [r10+" << git->second << "], rax\n";
            } else {
              if (valFloat) out_ << "  movsd [rbp - " << slot << "], xmm0\n";
              else out_ << "  mov [rbp - " << slot << "], rax\n";
            }
            break;
          }
          // compound assignment
          bool doFloat = valFloat || curFloat;
          if (doFloat) {
            (toGlobal ? globalIsFloat_ : slotIsFloat_)[id.name] = true;
            emitFOperand(*assign.value);       // xmm0 = value
            out_ << "  movsd xmm1, xmm0\n";    // xmm1 = value
            if (toGlobal) {
              out_ << "  lea r10, [rip+_a_globals]\n";
              out_ << "  movsd xmm0, [r10+" << git->second << "]\n";
            } else {
              out_ << "  movsd xmm0, [rbp - " << slot << "]\n";
            }
            std::string base = assign.op.size() > 1 ? assign.op.substr(0, assign.op.size()-1) : assign.op;
            if (base == "+") out_ << "  addsd xmm0, xmm1\n";
            else if (base == "-") out_ << "  subsd xmm0, xmm1\n";
            else if (base == "*") out_ << "  mulsd xmm0, xmm1\n";
            else if (base == "/") out_ << "  divsd xmm0, xmm1\n";
            else {
              // %, &, |, ^, <<, >> on floats: truncate both to int, int op
              out_ << "  cvttsd2si rax, xmm0\n  mov rbx, rax\n";
              out_ << "  cvttsd2si rax, xmm1\n  mov rcx, rax\n  mov rax, rbx\n";
              if (base == "%") { out_ << "  cqo\n  idiv rcx\n  mov rax, rdx\n"; }
              else if (base == "&") out_ << "  and rax, rcx\n";
              else if (base == "|") out_ << "  or rax, rcx\n";
              else if (base == "^") out_ << "  xor rax, rcx\n";
              else if (base == "<<") { out_ << "  mov rcx, rbx\n  shl rax, cl\n"; }
              else if (base == ">>") { out_ << "  mov rcx, rbx\n  sar rax, cl\n"; }
              // int result -> store as double
              out_ << "  cvtsi2sd xmm0, rax\n";
            }
            if (toGlobal) {
              out_ << "  lea r10, [rip+_a_globals]\n";
              out_ << "  movsd [r10+" << git->second << "], xmm0\n";
            } else {
              out_ << "  movsd [rbp - " << slot << "], xmm0\n";
            }
            break;
          }
          emitIOperand(*assign.value);
          if (toGlobal) {
            int off = git->second;
            {
              // rax = value; compute cur <op> value into rax, store back
              out_ << "  mov rbx, rax\n";
              out_ << "  lea r10, [rip+_a_globals]\n";
              out_ << "  mov rax, [r10+" << off << "]\n";
              const std::string& o = assign.op;
              std::string base = o.size() > 1 ? o.substr(0, o.size() - 1) : o;
              if (base == "+") out_ << "  add rax, rbx\n";
              else if (base == "-") out_ << "  sub rax, rbx\n";
              else if (base == "*") out_ << "  imul rax, rbx\n";
              else if (base == "/") { out_ << "  cqo\n  idiv rbx\n"; }
              else if (base == "%") { out_ << "  cqo\n  idiv rbx\n  mov rax, rdx\n"; }
              else if (base == "&") out_ << "  and rax, rbx\n";
              else if (base == "|") out_ << "  or rax, rbx\n";
              else if (base == "^") out_ << "  xor rax, rbx\n";
              else if (base == "<<") { out_ << "  mov rcx, rbx\n  shl rax, cl\n"; }
              else if (base == ">>") { out_ << "  mov rcx, rbx\n  sar rax, cl\n"; }
              out_ << "  mov [r10+" << off << "], rax\n";
            }
            break;
          }
          if (assign.op == "=") {
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "+=") {
            out_ << "  add [rbp - " << slot << "], rax\n";
          } else if (assign.op == "-=") {
            out_ << "  sub [rbp - " << slot << "], rax\n";
          } else if (assign.op == "*=") {
            out_ << "  mov rcx, [rbp - " << slot << "]\n";
            out_ << "  imul rax, rcx\n";
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "/=") {
            out_ << "  mov rbx, rax\n";
            out_ << "  mov rax, [rbp - " << slot << "]\n";
            out_ << "  cqo\n";
            out_ << "  idiv rbx\n";
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "%=") {
            out_ << "  mov rbx, rax\n";
            out_ << "  mov rax, [rbp - " << slot << "]\n";
            out_ << "  cqo\n";
            out_ << "  idiv rbx\n";
            out_ << "  mov [rbp - " << slot << "], rdx\n";
          } else if (assign.op == "&=") {
            out_ << "  and [rbp - " << slot << "], rax\n";
          } else if (assign.op == "|=") {
            out_ << "  or [rbp - " << slot << "], rax\n";
          } else if (assign.op == "^=") {
            out_ << "  xor [rbp - " << slot << "], rax\n";
          } else if (assign.op == "<<=") {
            out_ << "  mov rcx, rax\n";
            out_ << "  shl qword ptr [rbp - " << slot << "], cl\n";
          } else if (assign.op == ">>=") {
            out_ << "  mov rcx, rax\n";
            out_ << "  sar qword ptr [rbp - " << slot << "], cl\n";
          }
        }
        break;
      }
      case StmtKind::If: {
        const auto& ifStmt = static_cast<const IfStmt&>(stmt);
        if (ifStmt.branches.empty()) break;
        std::string endLbl = nextLabel("endif");

        for (std::size_t i = 0; i < ifStmt.branches.size(); ++i) {
          const auto& br = ifStmt.branches[i];
          emitExpr(*br.cond);
          out_ << "  cmp rax, 0\n";

          std::string nextArmLbl = (i + 1 < ifStmt.branches.size())
                                       ? nextLabel("elif")
                                       : (!ifStmt.elseBody.empty() ? nextLabel("else") : endLbl);
          out_ << "  je " << nextArmLbl << "\n";

          for (const auto& s : br.body) emitStmt(*s);
          out_ << "  jmp " << endLbl << "\n";

          if (i + 1 < ifStmt.branches.size() || !ifStmt.elseBody.empty()) {
            out_ << nextArmLbl << ":\n";
          }
        }

        if (!ifStmt.elseBody.empty()) {
          for (const auto& s : ifStmt.elseBody) emitStmt(*s);
        }

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::While: {
        const auto& wh = static_cast<const WhileStmt&>(stmt);
        std::string condLbl = nextLabel("while_cond");
        std::string endLbl = nextLabel("while_end");

        out_ << condLbl << ":\n";
        emitExpr(*wh.cond);
        out_ << "  cmp rax, 0\n";
        out_ << "  je " << endLbl << "\n";

        for (const auto& s : wh.body) emitStmt(*s);
        out_ << "  jmp " << condLbl << "\n";
        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::For: {
        const auto& forStmt = static_cast<const ForStmt&>(stmt);
        if (forStmt.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*forStmt.iterable);
          // The iterator lives in a hidden slot; the visible loop variable is
          // re-bound from it each iteration so body assignments to the loop
          // var cannot corrupt iteration (matches the interpreter).
          int iterSlot = allocateSlot("__iter_" + forStmt.var);
          int varSlot = allocateSlot(forStmt.var);
          emitIOperand(*range.start);
          out_ << "  mov [rbp - " << iterSlot << "], rax\n";

          int endSlot = allocateSlot("__end_" + forStmt.var);
          emitIOperand(*range.end);
          out_ << "  mov [rbp - " << endSlot << "], rax\n";

          std::string condLbl = nextLabel("for_cond");
          std::string bodyLbl = nextLabel("for_body");
          std::string endLbl = nextLabel("for_end");

          out_ << condLbl << ":\n";
          out_ << "  mov rax, [rbp - " << iterSlot << "]\n";
          out_ << "  cmp rax, [rbp - " << endSlot << "]\n";
          out_ << "  jge " << endLbl << "\n";
          out_ << "  mov [rbp - " << varSlot << "], rax\n";
          out_ << "  jmp " << bodyLbl << "\n";

          out_ << bodyLbl << ":\n";
          for (const auto& s : forStmt.body) emitStmt(*s);

          out_ << "  add qword ptr [rbp - " << iterSlot << "], 1\n";
          out_ << "  jmp " << condLbl << "\n";
          out_ << endLbl << ":\n";
        }
        break;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
          emitExpr(*ret.value);
        } else {
          out_ << "  mov rax, 0\n";
        }
        out_ << "  mov rsp, rbp\n";
        out_ << "  pop rbp\n";
        out_ << "  ret\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& es = static_cast<const ExprStmt&>(stmt);
        emitExpr(*es.expr);
        break;
      }
      default:
        break;
    }
  }

  void emitFOperand(const Expr& e) {
    emitExpr(e);
    if (!exprIsFloat(e)) {
      out_ << "  cvtsi2sd xmm0, rax\n";
    }
  }

  void emitIOperand(const Expr& e) {
    emitExpr(e);
    if (exprIsFloat(e)) {
      out_ << "  cvttsd2si rax, xmm0\n";
    }
  }

  void emitExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
        out_ << "  mov rax, " << static_cast<const IntLitExpr&>(expr).value << "\n";
        break;
      case ExprKind::BoolLit:
        out_ << "  mov rax, " << (static_cast<const BoolLitExpr&>(expr).value ? 1 : 0) << "\n";
        break;
      case ExprKind::FloatLit: {
        double v = static_cast<const FloatLitExpr&>(expr).value;
        out_ << "  movsd xmm0, [rip+" << floatTable_[v] << "]\n";
        break;
      }
      case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(expr);
        bool isF = exprIsFloat(expr);
        auto it = stackSlots_.find(id.name);
        if (it != stackSlots_.end()) {
          if (isF) out_ << "  movsd xmm0, [rbp - " << it->second << "]\n";
          else out_ << "  mov rax, [rbp - " << it->second << "]\n";
        } else {
          auto git = globalSlots_.find(id.name);
          if (git != globalSlots_.end()) {
            out_ << "  lea r10, [rip+_a_globals]\n";
            if (isF) out_ << "  movsd xmm0, [r10+" << git->second << "]\n";
            else out_ << "  mov rax, [r10+" << git->second << "]\n";
          } else if (isF) {
            out_ << "  xorpd xmm0, xmm0\n";
          } else {
            out_ << "  mov rax, 0\n";
          }
        }
        break;
      }
      case ExprKind::Binary: {
        const auto& bin = static_cast<const BinaryExpr&>(expr);
        static const std::set<std::string> cmpOps = {"==","!=","<","<=",">",">="};
        bool fbin = exprIsFloat(expr) ||
                    (cmpOps.count(bin.op) &&
                     (exprIsFloat(*bin.lhs) || exprIsFloat(*bin.rhs)));
        if (fbin) {
          emitFOperand(*bin.lhs);
          out_ << "  movsd xmm1, xmm0\n";
          emitFOperand(*bin.rhs);
          if (bin.op == "+") out_ << "  addsd xmm1, xmm0\n";
          else if (bin.op == "-") out_ << "  subsd xmm1, xmm0\n";
          else if (bin.op == "*") out_ << "  mulsd xmm1, xmm0\n";
          else if (bin.op == "/") out_ << "  divsd xmm1, xmm0\n";
          else if (bin.op == "%") {
            // fmod(lhs, rhs): xmm0=lhs, xmm1=rhs
            out_ << "  movsd xmm2, xmm0\n  movsd xmm0, xmm1\n  movsd xmm1, xmm2\n";
            out_ << "  call fmod@PLT\n";
          } else {
            // comparisons and bitwise ops on doubles: compare as double,
            // produce an i64 0/1 in rax
            out_ << "  ucomisd xmm1, xmm0\n";
            std::string setc = "setnp al";
            if (bin.op == "==") setc = "sete al";
            else if (bin.op == "!=") setc = "setne al";
            else if (bin.op == "<") setc = "setb al";
            else if (bin.op == "<=") setc = "setbe al";
            else if (bin.op == ">") setc = "seta al";
            else if (bin.op == ">=") setc = "setae al";
            else {
              // bitwise on floats: convert to int and redo as int op
              out_ << "  cvttsd2si rax, xmm1\n  mov rbx, rax\n";
              out_ << "  cvttsd2si rax, xmm0\n  mov rcx, rax\n  mov rax, rbx\n";
              std::string bop = bin.op;
              if (bop == "and" || bop == "&") out_ << "  and rax, rcx\n";
              else if (bop == "|") out_ << "  or rax, rcx\n";
              else if (bop == "^") out_ << "  xor rax, rcx\n";
              else if (bop == "<<") { out_ << "  mov rcx, rbx\n"; }
              goto fbin_done;
            }
            out_ << "  " << setc << "\n  movzx rax, al\n";
          }
          if (bin.op != "%") out_ << "  movsd xmm0, xmm1\n";
          goto fbin_done;
        }
        emitIOperand(*bin.lhs);
        out_ << "  push rax\n";
        emitIOperand(*bin.rhs);
        out_ << "  mov rbx, rax\n";
        out_ << "  pop rax\n";

        if (bin.op == "+") {
          out_ << "  add rax, rbx\n";
        } else if (bin.op == "-") {
          out_ << "  sub rax, rbx\n";
        } else if (bin.op == "*") {
          out_ << "  imul rax, rbx\n";
        } else if (bin.op == "/") {
          out_ << "  cqo\n";
          out_ << "  idiv rbx\n";
        } else if (bin.op == "%") {
          out_ << "  cqo\n";
          out_ << "  idiv rbx\n";
          out_ << "  mov rax, rdx\n";
        } else if (bin.op == "==") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  sete al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "!=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setne al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "<") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setl al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "<=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setle al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == ">") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setg al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == ">=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setge al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "and" || bin.op == "&") {
          out_ << "  and rax, rbx\n";
        } else if (bin.op == "or" || bin.op == "|") {
          out_ << "  or rax, rbx\n";
        } else if (bin.op == "^") {
          out_ << "  xor rax, rbx\n";
        } else if (bin.op == "<<") {
          out_ << "  mov rcx, rbx\n";
          out_ << "  shl rax, cl\n";
        } else if (bin.op == ">>") {
          out_ << "  mov rcx, rbx\n";
          out_ << "  sar rax, cl\n";
        }
        break;
      fbin_done:
        break;
      }
      case ExprKind::Unary: {
        const auto& un = static_cast<const UnaryExpr&>(expr);
        if (un.op == "-" && exprIsFloat(*un.operand)) {
          emitExpr(*un.operand);
          if (!exprIsFloat(*un.operand)) out_ << "  cvtsi2sd xmm0, rax\n";
          out_ << "  mov rax, 0x8000000000000000\n";
          out_ << "  movq xmm1, rax\n";
          out_ << "  xorpd xmm0, xmm1\n";
          break;
        }
        emitIOperand(*un.operand);
        if (un.op == "-") {
          out_ << "  neg rax\n";
        } else if (un.op == "~") {
          out_ << "  not rax\n";
        } else if (un.op == "not" || un.op == "!") {
          out_ << "  cmp rax, 0\n";
          out_ << "  sete al\n";
          out_ << "  movzx rax, al\n";
        }
        break;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*call.callee).name;
          if (name == "print") {
            if (!call.args.empty()) {
              const auto& argExpr = *call.args[0].value;
              if (exprIsFloat(argExpr)) {
                emitFOperand(argExpr);
                out_ << "  lea rdi, .Lfmt_float[rip]\n";
                out_ << "  mov eax, 1\n";
                out_ << "  call printf@PLT\n";
                return;
              }
              if (argExpr.kind == ExprKind::StringLit) {
                std::string s = extractString(static_cast<const StringLitExpr&>(argExpr));
                out_ << "  lea rdi, .Lfmt_str[rip]\n";
                out_ << "  lea rsi, " << strTable_[s] << "[rip]\n";
                out_ << "  mov rax, 0\n";
                out_ << "  call printf@PLT\n";
                return;
              }
              emitIOperand(argExpr);
              out_ << "  mov rsi, rax\n";
              out_ << "  lea rdi, .Lfmt_int[rip]\n";
              out_ << "  mov rax, 0\n";
              out_ << "  call printf@PLT\n";
              return;
            }
          }

          // Float/math builtins (libm, xmm0/xmm1 convention)
          static const std::set<std::string> libm1 = {
              "floor","ceil","round","exp","log","sin","cos","tan","fabs"};
          if (name == "sqrt" && !call.args.empty()) {
            emitFOperand(*call.args[0].value);
            out_ << "  sqrtsd xmm0, xmm0\n";
            return;
          }
          if (libm1.count(name) && !call.args.empty()) {
            emitFOperand(*call.args[0].value);
            out_ << "  call " << name << "@PLT\n";
            return;
          }
          if ((name == "pow" || name == "fmod") && call.args.size() >= 2) {
            emitFOperand(*call.args[0].value);
            out_ << "  movsd xmm2, xmm0\n";
            emitFOperand(*call.args[1].value);
            out_ << "  movsd xmm1, xmm0\n  movsd xmm0, xmm2\n";
            out_ << "  call " << name << "@PLT\n";
            return;
          }
          if ((name == "f64" || name == "float") && !call.args.empty()) {
            emitIOperand(*call.args[0].value);   // rax = int bits or converted
            if (exprIsFloat(*call.args[0].value)) {
              // arg was double: cvttsd2si already truncated; redo properly
              emitFOperand(*call.args[0].value);
            } else {
              out_ << "  cvtsi2sd xmm0, rax\n";
            }
            return;
          }
          if (name == "int" && !call.args.empty()) {
            emitFOperand(*call.args[0].value);
            if (!exprIsFloat(*call.args[0].value)) {
              // int(int) — value already in rax via emitIOperand path; but
              // emitFOperand left xmm0; convert back
              out_ << "  cvttsd2si rax, xmm0\n";
            } else {
              out_ << "  cvttsd2si rax, xmm0\n";
            }
            return;
          }
          // User function call
          static const char* kArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
          for (std::size_t i = 0; i < call.args.size() && i < 6; ++i) {
            emitExpr(*call.args[i].value);
            out_ << "  push rax\n";
          }
          for (int i = static_cast<int>(call.args.size()) - 1; i >= 0 && i < 6; --i) {
            out_ << "  pop " << kArgRegs[i] << "\n";
          }
          out_ << "  call " << (name == "main" ? "_a_main" : "_a_" + name) << "\n";
        }
        break;
      }
      default:
        out_ << "  mov rax, 0\n";
        break;
    }
  }

  void emitMain() {
    out_ << ".globl main\n";
    out_ << ".type main, @function\n";
    out_ << "main:\n";
    out_ << "  push rbp\n";
    out_ << "  mov rbp, rsp\n";
    out_ << "  sub rsp, 256\n";

    stackSlots_.clear();
    slotIsFloat_.clear();
    currentStackOffset_ = 0;
    inMain_ = true;

    if (graph_.entry >= 0 && static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) {
            emitStmt(*stmt);
          }
        }
      }
    }

    out_ << "  mov rax, 0\n";
    out_ << "  mov rsp, rbp\n";
    out_ << "  pop rbp\n";
    out_ << "  ret\n";
  }
};

}  // namespace

bool emitLLVMIR(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  LLVMEmitter emitter(graph, sema, out);
  return emitter.emit();
}

bool emitX86Assembly(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  // The asm emitter cannot represent float args/returns on user calls; the C
  // emitter (used by the JIT and `a build`) can. Refuse rather than miscompile.
  if (!nativeFloatScanOk(graph)) return false;
  X86Emitter emitter(graph, sema, out);
  return emitter.emit();
}

bool buildLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                      const std::string& outputPath, std::ostream& err) {
  // The Astra (compiler/astra/) is the ONLY native compiler for A:
  // A -> emitCSource() (C99 aval ABI) -> in-process native EXE output.
  // No external gcc/clang/llc subprocess is invoked. See tcc_backend.cpp.
  return kgen::buildExecutableWithNative(graph, sema, outputPath, err);
}

bool compileLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& outputBase, std::ostream& err) {
  return buildLLVMModule(graph, sema, outputBase, err);
}

int runBuiltLLVMModule(const std::string& tmpBase, std::istream& in, std::ostream& out,
                       std::ostream& err) {
  (void)in;
  // Execute directly and stream stdout/stderr
  std::string runCmd = tmpBase + " 2>/tmp/a_llvm_err.txt";
  FILE* pipe = popen(runCmd.c_str(), "r");
  if (!pipe) {
    err << "error: failed to execute LLVM binary\n";
    std::filesystem::remove(tmpBase);
    std::filesystem::remove(tmpBase + ".s");
    std::filesystem::remove(tmpBase + ".ll");
    return 2;
  }

  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    out << buf;
  }
  int status = pclose(pipe);

  std::ifstream errFile("/tmp/a_llvm_err.txt");
  if (errFile) {
    std::string errLine;
    while (std::getline(errFile, errLine)) {
      err << errLine << "\n";
    }
  }
  std::filesystem::remove("/tmp/a_llvm_err.txt");
  std::filesystem::remove(tmpBase);
  std::filesystem::remove(tmpBase + ".s");
  std::filesystem::remove(tmpBase + ".ll");

  return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

int runLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                  std::istream& in, std::ostream& out, std::ostream& err) {
  std::string tmpBase = "/tmp/a_llvm_run_" + std::to_string(getpid()) + "_" + std::to_string(rand());
  if (!compileLLVMModule(graph, sema, tmpBase, err)) {
    return 1;
  }
  return runBuiltLLVMModule(tmpBase, in, out, err);
}

namespace {
// stdlib/prelude modules (stdlib/**, incl. the auto-loaded prelude) use
// language features the native subset cannot lower; they are never emitted,
// so they must not gate user programs either.
bool isStdlibModule(const ModuleGraph& graph, const Module& mod) {
  // The entry module is user code even when its path happens to contain a
  // "stdlib" directory component (e.g. tests/integration/cases/stdlib/).
  if (mod.id == graph.entry) return false;
  return mod.path.find("stdlib") != std::string::npos;
}
bool checkSupportedExpr(const Expr& expr);
bool checkSupportedStmt(const Stmt& stmt, bool isGlobal = false) {
  auto fail = [&](const char* what) {
    if (std::getenv("A_DEBUG_NATIVE"))
      std::fprintf(stderr, "[native-gate] unsupported %s (stmt kind %d, global %d)\n",
                   what, static_cast<int>(stmt.kind), static_cast<int>(isGlobal));
    return false;
  };
  switch (stmt.kind) {
    case StmtKind::Let: {
      if (isGlobal) return false;
      const auto& s = static_cast<const LetStmt&>(stmt);
      return !s.init || checkSupportedExpr(*s.init);
    }
    case StmtKind::Assign: {
      const auto& s = static_cast<const AssignStmt&>(stmt);
      if (s.target->kind == ExprKind::Index) {
        const auto& ix = static_cast<const IndexExpr&>(*s.target);
        return checkSupportedExpr(*ix.object) && checkSupportedExpr(*ix.index) &&
               checkSupportedExpr(*s.value);
      }
      return s.target->kind == ExprKind::Ident && checkSupportedExpr(*s.value);
    }
    case StmtKind::Expr: {
      const auto& s = static_cast<const ExprStmt&>(stmt);
      return checkSupportedExpr(*s.expr);
    }
    case StmtKind::Return: {
      const auto& s = static_cast<const ReturnStmt&>(stmt);
      return !s.value || checkSupportedExpr(*s.value);
    }
    case StmtKind::If: {
      const auto& s = static_cast<const IfStmt&>(stmt);
      for (const auto& br : s.branches) {
        if (!checkSupportedExpr(*br.cond)) return false;
        for (const auto& st : br.body) if (!checkSupportedStmt(*st, false)) return false;
      }
      for (const auto& st : s.elseBody) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::While: {
      const auto& s = static_cast<const WhileStmt&>(stmt);
      if (!checkSupportedExpr(*s.cond)) return false;
      for (const auto& st : s.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::For: {
      const auto& s = static_cast<const ForStmt&>(stmt);
      if (s.iterable->kind == ExprKind::Range) {
        const auto& r = static_cast<const RangeExpr&>(*s.iterable);
        if (!checkSupportedExpr(*r.start) || !checkSupportedExpr(*r.end)) return false;
        if (r.step && !checkSupportedExpr(*r.step)) return false;
      } else if (!checkSupportedExpr(*s.iterable)) {
        return false;  // lists/strings iterate via a_index in the C emitter
      }
      // Scoped shadowing check: if any nested statement declares a variable with loop var's name,
      // fallback to interpreter
      std::vector<const Stmt*> queue;
      for (const auto& st : s.body) queue.push_back(st.get());
      while (!queue.empty()) {
        const Stmt* cur = queue.back();
        queue.pop_back();
        if (cur->kind == StmtKind::Let) {
          const auto& let = static_cast<const LetStmt&>(*cur);
          if (let.name == s.var) return false;
        } else if (cur->kind == StmtKind::If) {
          const auto& ifs = static_cast<const IfStmt&>(*cur);
          for (const auto& br : ifs.branches) {
            for (const auto& st : br.body) queue.push_back(st.get());
          }
          for (const auto& st : ifs.elseBody) queue.push_back(st.get());
        } else if (cur->kind == StmtKind::While) {
          const auto& wh = static_cast<const WhileStmt&>(*cur);
          for (const auto& st : wh.body) queue.push_back(st.get());
        }
      }
      for (const auto& st : s.body) {
        if (!checkSupportedStmt(*st, false)) return false;
      }
      return true;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      return true;  // emitted as C break/continue inside loop bodies
    case StmtKind::FnDecl: {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      // If function assigns to a global (defined at top-level), fallback (globals not yet implemented in LLVM/native without global storage)
      // Collect globals from graph not possible here without graph ref; instead: if function has Assign to a name that is not a param and not Let in same fn, treat as unsupported.
      // We approximate by checking: any Assign target name that never appears as Let or param in fn is a global mutation.
      {
        std::set<std::string> localNames;
        for (auto& prm: fn.params) localNames.insert(prm.name);
        // collect Lets recursively via stack
        std::vector<const Stmt*> q; for (auto& s: fn.body) q.push_back(s.get());
        while(!q.empty()){auto* cur=q.back(); q.pop_back();
          if(cur->kind==StmtKind::Let){ localNames.insert(static_cast<const LetStmt&>(*cur).name); }
          // In A's Python-style semantics the first `name = value` inside a
          // function declares a local, so assign targets count as locals too.
          else if(cur->kind==StmtKind::Assign){ auto& a=static_cast<const AssignStmt&>(*cur); if(a.target->kind==ExprKind::Ident) localNames.insert(static_cast<const IdentExpr&>(*a.target).name); }
          else if(cur->kind==StmtKind::If){ auto& ifs=static_cast<const IfStmt&>(*cur); for(auto& br: ifs.branches) for(auto& s: br.body) q.push_back(s.get()); for(auto& s: ifs.elseBody) q.push_back(s.get()); }
          else if(cur->kind==StmtKind::While){ auto& wh=static_cast<const WhileStmt&>(*cur); for(auto& s: wh.body) q.push_back(s.get()); }
          else if(cur->kind==StmtKind::For){ auto& fo=static_cast<const ForStmt&>(*cur); localNames.insert(fo.var); for(auto& s: fo.body) q.push_back(s.get()); }
        }
        // now check assigns
        std::vector<const Stmt*> q2; for (auto& s: fn.body) q2.push_back(s.get());
        while(!q2.empty()){auto* cur=q2.back(); q2.pop_back();
          if(cur->kind==StmtKind::Assign){ auto& a=static_cast<const AssignStmt&>(*cur); if(a.target->kind==ExprKind::Ident){ auto& id=static_cast<const IdentExpr&>(*a.target); if(localNames.count(id.name)==0) return false; } }
          else if(cur->kind==StmtKind::If){ auto& ifs=static_cast<const IfStmt&>(*cur); for(auto& br: ifs.branches) for(auto& s: br.body) q2.push_back(s.get()); for(auto& s: ifs.elseBody) q2.push_back(s.get()); }
          else if(cur->kind==StmtKind::While){ auto& wh=static_cast<const WhileStmt&>(*cur); for(auto& s: wh.body) q2.push_back(s.get()); }
          else if(cur->kind==StmtKind::For){ auto& fo=static_cast<const ForStmt&>(*cur); for(auto& s: fo.body) q2.push_back(s.get()); }
        }
      }
      for (const auto& st : fn.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    default:
      return fail("stmt");
  }
}

// True when the expression is provably a double at runtime (contains a float
// literal or a float builtin), making / and % safe from integer div-by-zero.
bool exprProvablyFloat(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::FloatLit:
      return true;
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      static const std::set<std::string> arith = {"+","-","*","/","%"};
      return arith.count(b.op) &&
             (exprProvablyFloat(*b.lhs) || exprProvablyFloat(*b.rhs));
    }
    case ExprKind::Unary:
      return static_cast<const UnaryExpr&>(expr).op == "-" &&
             exprProvablyFloat(*static_cast<const UnaryExpr&>(expr).operand);
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(expr);
      if (c.callee->kind != ExprKind::Ident) return false;
      static const std::set<std::string> fns = {
          "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos",
          "tan","fabs","f64","float"};
      return fns.count(static_cast<const IdentExpr&>(*c.callee).name) != 0;
    }
    default:
      return false;
  }
}

bool checkSupportedExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
      return true;
    case ExprKind::BoolLit:
      return false;  // bool literals print/behave differently natively
    case ExprKind::Ident: {
      // Stream constants only exist in the interpreter and C emitter.
      const std::string ident = static_cast<const IdentExpr&>(expr).name;
      return ident != "stdout" && ident != "stderr";
    }
    case ExprKind::ArrayLit: {
      const auto& a = static_cast<const ArrayLitExpr&>(expr);
      for (const auto& el : a.elements)
        if (!checkSupportedExpr(*el)) return false;
      return true;
    }
    case ExprKind::Index: {
      const auto& x = static_cast<const IndexExpr&>(expr);
      return checkSupportedExpr(*x.object) && checkSupportedExpr(*x.index);
    }
    case ExprKind::Comprehension: {
      const auto& cp = static_cast<const ComprehensionExpr&>(expr);
      if (cp.isSet) return false;  // sets need a hash runtime
      return checkSupportedExpr(*cp.element) && checkSupportedExpr(*cp.iterable) &&
             (!cp.condition || checkSupportedExpr(*cp.condition));
    }
    case ExprKind::MapLit: {
      const auto& m = static_cast<const MapLitExpr&>(expr);
      for (const auto& kv : m.entries)
        if (!checkSupportedExpr(*kv.first) || !checkSupportedExpr(*kv.second))
          return false;
      return true;
    }
    case ExprKind::Lambda: {
      const auto& l = static_cast<const LambdaExpr&>(expr);
      return checkSupportedExpr(*l.body);
    }
    case ExprKind::StringLit: {
      // Static string literals are heap objects in the C emitter; f-strings
      // (interpolated parts) still need the interpreter.
      const auto& sl = static_cast<const StringLitExpr&>(expr);
      for (const auto& part : sl.parts) if (part.isExpr) return false;
      return true;
    }
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      // Division/remainder are safe natively: the C runtime helpers check
      // for zero divisors and exit(2) like the interpreter.
      if (b.op == "<<" || b.op == ">>") {
        if (b.rhs->kind == ExprKind::IntLit) {
          int64_t val = static_cast<const IntLitExpr&>(*b.rhs).value;
          if (val < 0 || val >= 64) return false;
        } else {
          return false;
        }
      }
      return checkSupportedExpr(*b.lhs) && checkSupportedExpr(*b.rhs);
    }
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(expr);
      if (u.op == "!") return false; // unwrap is handled by runtime/interpreter
      return checkSupportedExpr(*u.operand);
    }
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(expr);
      if (c.callee->kind == ExprKind::Field) {
        // Method calls: only list.append is supported natively.
        const auto& f = static_cast<const FieldExpr&>(*c.callee);
        if (f.field != "append" || c.args.size() != 1) return false;
        return checkSupportedExpr(*f.object) && checkSupportedExpr(*c.args[0].value);
      }
      if (c.callee->kind != ExprKind::Ident) return false;
      const std::string& name = static_cast<const IdentExpr&>(*c.callee).name;
      // Standard-library builtins (json, binary, testing, log, metrics,
      // trace, net, http) only exist in the interpreter.
      if (isStdlibBuiltin(name)) return false;
      // Native runtime functions are handled by interpreter or native backend
      if (name == "open" || name == "close" ||
          name == "write_file" || name == "read_file" || name == "append_file" ||
          name == "file_exists" || name == "remove_file" || name == "flush" ||
          name == "input" || name == "read_line" ||
          name == "int" || name == "float" || name == "set" ||
          name == "unsafe_get" || name == "unsafe_set" || name == "move" ||
          name == "arena_new" || name == "arena_pool" || name == "arena_region" ||
          name == "arena_alloc" || name == "arena_release" || name == "arena_reset" ||
          name == "arena_used" || name == "arena_total" || name == "copy" ||
          name == "borrow" || name == "borrow_mut" || name == "unsafe_address_of" ||
          name == "unsafe_load" || name == "unsafe_store" || name == "unsafe_offset") {
        return false;
      }
      if (name == "print" || name == "eprint" || name == "ic" || name == "__file__" ||
          name == "__line__" || name == "__func__" || name == "__function__" ||
          name == "__source_file__" || name == "__source_line__" || name == "__repr__" ||
          name == "__debug_repr__" || name == "__debug_time__" || name == "__debug_context__" ||
          name == "__ic_format__") {
        // print/eprint/ic/debug intrinsics have dynamic formatting/context semantics.
        // Run the whole program in the interpreter.
        return false;
      }
      for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
      return true;
    }
    default:
      return false;
  }
}
// ============================================================================
// Whole-program float-safety analysis for the native subset.
// ============================================================================
// Native functions pass and return raw i64 in registers/slots; only the
// emitter's float builtins produce doubles. A user-function call whose
// argument or result is float-typed would silently miscompile (the value is
// reinterpreted, not converted), so programs containing one are rejected here
// and handled by the interpreter instead.
class NativeFloatScan {
 public:
  explicit NativeFloatScan(const ModuleGraph& graph) : graph_(graph) {
    collectGlobalNames();
    pretypeGlobals();
  }

  bool ok() const { return ok_; }

 private:
  const ModuleGraph& graph_;
  bool ok_ = true;
  std::set<std::string> globalNames_;
  std::unordered_map<std::string, bool> globals_;

  static std::string cn(const std::string& n) { return n; }

  void collectGlobalNames() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      if (stmt->kind == StmtKind::Let) {
        globalNames_.insert(static_cast<const LetStmt&>(*stmt).name);
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        globalNames_.insert(
            static_cast<const IdentExpr&>(*static_cast<const AssignStmt&>(*stmt).target).name);
      }
    }
  }

  bool exprIsFloatCallName(const std::string& name) {
    static const std::set<std::string> fns = {
        "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos","tan",
        "fabs","f64","float"};
    return fns.count(name) != 0;
  }

  bool isUserCallName(const std::string& name) {
    static const std::set<std::string> builtin = {
        "print","sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos",
        "tan","fabs","f64","float","int"};
    return !builtin.count(name) && !isStdlibBuiltin(name);
  }

  // Mirrors the emitters' exprIsFloat: arith on a float operand is float,
  // comparisons and everything else are int.
  bool exprFloat(const Expr& e, const std::unordered_map<std::string, bool>& slot) {
    switch (e.kind) {
      case ExprKind::FloatLit: return true;
      case ExprKind::Ident: {
        std::string n = cn(static_cast<const IdentExpr&>(e).name);
        auto it = slot.find(n);
        if (it != slot.end()) return it->second;
        auto gi = globals_.find(n);
        return gi != globals_.end() && gi->second;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        static const std::set<std::string> arith = {"+","-","*","/","%"};
        return arith.count(b.op) && (exprFloat(*b.lhs, slot) || exprFloat(*b.rhs, slot));
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        return u.op == "-" && exprFloat(*u.operand, slot);
      }
      case ExprKind::Call:
        return exprIsFloatCallName(
            static_cast<const IdentExpr&>(*static_cast<const CallExpr&>(e).callee).name);
      default: return false;
    }
  }

  void checkExpr(const Expr& e, const std::unordered_map<std::string, bool>& slot) {
    if (!ok_) return;
    switch (e.kind) {
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        checkExpr(*b.lhs, slot);
        checkExpr(*b.rhs, slot);
        break;
      }
      case ExprKind::Unary:
        checkExpr(*static_cast<const UnaryExpr&>(e).operand, slot);
        break;
      case ExprKind::Range: {
        const auto& r = static_cast<const RangeExpr&>(e);
        checkExpr(*r.start, slot);
        checkExpr(*r.end, slot);
        if (r.step) checkExpr(*r.step, slot);
        break;
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*c.callee).name;
          if (isUserCallName(name)) {
            for (const auto& a : c.args) {
              if (exprFloat(*a.value, slot)) { ok_ = false; return; }
              checkExpr(*a.value, slot);
            }
            return;
          }
        }
        for (const auto& a : c.args) checkExpr(*a.value, slot);
        break;
      }
      default:
        break;
    }
  }

  // Walks statements in order tracking per-name floatness, mirroring how the
  // emitters update slotIsFloat_/globalIsFloat_.
  void scanStmts(const std::vector<StmtPtr>& body,
                 std::unordered_map<std::string, bool>& slot, bool inFn) {
    for (const auto& sp : body) {
      if (!ok_) return;
      const Stmt& s = *sp;
      switch (s.kind) {
        case StmtKind::Let: {
          const auto& l = static_cast<const LetStmt&>(s);
          std::string n = cn(l.name);
          if (l.init) {
            checkExpr(*l.init, slot);
            bool fv = exprFloat(*l.init, slot);
            if (inFn || !globalNames_.count(n)) slot[n] = fv;
            else globals_[n] = fv;
          }
          break;
        }
        case StmtKind::Assign: {
          const auto& a = static_cast<const AssignStmt&>(s);
          checkExpr(*a.value, slot);
          if (a.target->kind == ExprKind::Ident) {
            std::string n = cn(static_cast<const IdentExpr&>(*a.target).name);
            bool fv = exprFloat(*a.value, slot);
            bool isG = globalNames_.count(n) && !slot.count(n);
            if (a.op == "=") {
              if (isG) globals_[n] = fv; else slot[n] = fv;
            } else {
              bool cur = isG ? globals_[n] : slot[n];
              if (fv || cur) { if (isG) globals_[n] = true; else slot[n] = true; }
            }
          }
          break;
        }
        case StmtKind::Expr:
          checkExpr(*static_cast<const ExprStmt&>(s).expr, slot);
          break;
        case StmtKind::Return: {
          const auto& r = static_cast<const ReturnStmt&>(s);
          if (r.value) {
            checkExpr(*r.value, slot);
            // A float return would be reinterpreted as int by callers.
            if (inFn && exprFloat(*r.value, slot)) ok_ = false;
          }
          break;
        }
        case StmtKind::If: {
          const auto& i = static_cast<const IfStmt&>(s);
          for (const auto& br : i.branches) {
            checkExpr(*br.cond, slot);
            scanStmts(br.body, slot, inFn);
          }
          scanStmts(i.elseBody, slot, inFn);
          break;
        }
        case StmtKind::While: {
          const auto& w = static_cast<const WhileStmt&>(s);
          checkExpr(*w.cond, slot);
          scanStmts(w.body, slot, inFn);
          break;
        }
        case StmtKind::For: {
          const auto& f = static_cast<const ForStmt&>(s);
          if (f.iterable->kind == ExprKind::Range) {
            const auto& r = static_cast<const RangeExpr&>(*f.iterable);
            checkExpr(*r.start, slot);
            checkExpr(*r.end, slot);
            if (r.step) checkExpr(*r.step, slot);
          } else {
            checkExpr(*f.iterable, slot);
          }
          std::string v = cn(f.var);
          slot[v] = false;
          slot["__iter_" + v] = false;
          slot["__end_" + v] = false;
          scanStmts(f.body, slot, inFn);
          break;
        }
        case StmtKind::FnDecl:
          break;  // scanned separately with its own scope
        default:
          break;
      }
    }
  }

  void pretypeGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    std::unordered_map<std::string, bool> slot;
    for (const auto& stmt : root.program->statements) {
      // Only typing matters here; call checks run in the main scan below.
      if (stmt->kind == StmtKind::Let) {
        const auto& l = static_cast<const LetStmt&>(*stmt);
        if (l.init && globalNames_.count(cn(l.name)))
          globals_[cn(l.name)] = exprFloat(*l.init, slot);
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        const auto& a = static_cast<const AssignStmt&>(*stmt);
        std::string n = cn(static_cast<const IdentExpr&>(*a.target).name);
        if (!globalNames_.count(n)) continue;
        bool fv = exprFloat(*a.value, slot);
        if (a.op == "=") globals_[n] = fv;
        else globals_[n] = fv || globals_[n];
      }
    }
  }

 public:
  void run() {
    // Functions first: each gets a fresh local-type environment.
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModule(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind != StmtKind::FnDecl) continue;
        const auto& fn = static_cast<const FnDecl&>(*stmt);
        std::unordered_map<std::string, bool> slot;
        for (const auto& p : fn.params) slot[cn(p.name)] = false;
        scanStmts(fn.body, slot, true);
      }
    }
    // Then top level (main): globals + main locals.
    if (graph_.entry >= 0 &&
        static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        std::unordered_map<std::string, bool> slot;
        scanStmts(root.program->statements, slot, false);
      }
    }
  }
};

bool nativeFloatScanOk(const ModuleGraph& graph) {
  NativeFloatScan scan(graph);
  scan.run();
  return scan.ok();
}

}  // namespace

bool isLLVMSupported(const ModuleGraph& graph) {
  if (graph.modules.empty()) return false;
  for (const auto& mod : graph.modules) {
    if (!mod.program || isStdlibModule(graph,mod)) continue;
    for (const auto& st : mod.program->statements) {
      if (!checkSupportedStmt(*st, true)) return false;
    }
  }
  return true;
}

}  // namespace a
