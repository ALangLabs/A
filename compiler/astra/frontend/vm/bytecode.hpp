#pragma once

// Slot-based bytecode VM for the "--vm" interpreter. Compiles each function's
// AST to a fixed-register instruction stream and runs a fetch-decode-execute
// loop over a shared slot arena — no per-iteration scope allocation, no
// string lookups. Coexists with the tree-walker in vm.cpp: runProgram tries
// bytecode first and falls back to the AST walker when any construct is
// unsupported.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/fwd.hpp"

namespace a {
struct ModuleGraph;
struct SemaResult;
struct FnDecl;
struct LambdaExpr;
struct Stmt;
}  // namespace a

namespace a::vm::bc {

// aval is uint64_t (a_runtime.h); keep the header C++-only.
using aval = std::uint64_t;

enum class Op : std::uint8_t {
  LoadConst,    // a=dst, b=consts idx
  LoadNone,     // a=dst
  LoadTrue,     // a=dst
  LoadFalse,    // a=dst
  LoadGlobal,   // a=dst, b=global slot idx (this module)
  StoreGlobal,  // a=global slot idx, b=src slot
  LoadBuiltin,  // a=dst, b=runtime builtin id
  Move,         // a=dst, b=src

  // binary: a=dst, b=lhs slot, c=rhs slot
  Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
  Eq, Ne, Lt, Le, Gt, Ge,
  BitAnd, BitOr, BitXor, Shl, Shr,
  In,           // a=dst, b=item, c=container -> a_contains
  Neg, Not, BNot, Unwrap,  // a=dst, b=src

  Jump,         // a=target ip
  JumpIfFalse,  // a=src slot, b=target ip
  JumpIfTrue,   // a=src slot, b=target ip

  Call,         // a=dst, b=fn slot, c=argBase slot, d=nargs
  CallKnown,    // a=dst, b=code idx, c=argBase slot, d=nargs (direct bc fn)
  CallBuiltin,  // a=dst, b=runtime id, c=argBase slot, d=nargs
  MethodCall,   // a=dst, b=base slot (obj,args..), c=names idx, d=nargs, e=nkw

  Return,       // a=src slot (-1 -> A_NONE)

  NewList,      // a=dst, b=elemBase, c=count
  NewTuple,     // a=dst, b=elemBase, c=count
  NewSet,       // a=dst, b=elemBase, c=count
  NewDict,      // a=dst, b=pairBase, c=slot count (k,v pairs)
  GetItem,      // a=dst, b=obj, c=idx
  SetItem,      // a=obj, b=idx, c=val
  GetField,     // a=dst, b=obj, c=names idx
  SetField,     // a=obj, b=names idx, c=val
  Slice,        // a=dst, b=base (obj,lo,hi,step consecutive), c=4
  StrJoin,      // a=dst, b=segBase, c=count -> a_str_join
  MakeRange,    // a=dst, b=lo, c=hi, d=step

  IterInit,     // a=dst iter, b=src iterable
  IterNext,     // a=item dst, b=iter slot, c=end target ip
  ListAppend,   // a=list slot, b=value -> a_list_push
  SetAdd,       // a=set slot, b=value -> a_set_add

  Closure,      // a=dst, b=code idx, c=0 local / 1 global slot idx=a
                //   captures code.upvals parent slots into env[2..]
  BindModule,   // a=global slot, b=display name idx, c=moduleId (-1 native),
                // d=native name idx (-1 none)
  ImportMember, // a=global slot, b=src moduleId, c=name idx
  MakeCell,     // a=slot: wrap sl[a] in a 1-cell env (captured var)
  CellGet,      // a=dst, b=cell slot -> sl[a] = env_get(sl[b],0)
  CellSet,      // a=cell slot, b=src -> env_set(sl[a],0,sl[b])
  GetUpval,     // a=dst, b=upval idx -> sl[a] = env_get(curEnv,2+b)
  SetUpval,     // a=upval idx, b=src -> env_set(curEnv,2+a,sl[b])
  TryQ,         // a=dst, b=src: a_try_result + propagate check (`expr?`)
  SafeNav,      // a=dst, b=obj, c=name idx, d=fieldOptional (`obj?.field`)
  PushDefer,    // a=defer list slot, b=closure slot
  RunDefers,    // a=defer list slot: run contents LIFO, reset to None
  MatchFail,    // a=subj slot: no arm matched -> a_match_fail
};

struct Instr {
  Op op;
  std::int32_t a = 0, b = 0, c = 0, d = 0, e = 0;
};

struct Code {
  std::vector<Instr> ins;
  std::vector<aval> consts;
  std::vector<std::string> names;  // string operands for Get/SetField etc.
  std::vector<std::string> paramNames;
  std::vector<std::int32_t> paramDefaults;  // consts idx, -1 = A_NONE
  std::vector<std::uint32_t> upvals;        // parent frame slot per upvalue
  std::vector<std::pair<int, int>> deferSlots;  // (slot, scope depth) for unwind
  std::string name;                        // fn name (tracebacks)
  int line = 0;                            // decl line (tracebacks)
  int frameSize = 0;
  int moduleId = -1;
  bool variadic = false;
  bool isMain = false;
};

struct BcModule {
  std::vector<aval> vars;                    // global slots
  std::unordered_map<std::string, int> slotOf;
};

struct State {
  std::deque<Code> codes;  // deque: Code& stays valid across emplace_back
  std::vector<BcModule> modules;
  std::vector<int> moduleMain;  // moduleId -> codes idx of its main code (-1)
  std::unordered_map<const a::LambdaExpr*, std::uint32_t> lambdaCode;
  std::unordered_map<const a::Stmt*, std::uint32_t> anonCode;
  std::unordered_map<aval, std::uint32_t> fnInfo;  // fn aval -> code idx
  std::unordered_map<aval, aval> fnEnv;           // fn aval -> closure env
  std::unordered_map<aval, int> moduleOf;         // marker -> moduleId
  std::unordered_map<aval, std::string> nativeOf;
  std::unordered_map<aval, std::string> pyModOf;
  std::unordered_map<const FnDecl*, std::uint32_t> declCode;
};

// Compile the whole program. Returns false when any construct is
// unsupported — caller then falls back to the tree-walker.
bool compileAll(const ModuleGraph& graph, const SemaResult& sema);

// Execute the compiled program (modules in dependency order, then entry /
// tests). Only call after compileAll returned true. Returns exit code.
int execute(const ModuleGraph& graph, bool testMode,
            const std::string& testFilter);

// GC root marking (registered via a_gc_add_root_cb).


}  // namespace a::vm::bc
