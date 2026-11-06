#include <iostream>
#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "ir/ir.hpp"
#include "name-resolution/module_loader.hpp"
#include "optimization/optimizer.hpp"
#include "type-checker/sema.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

struct Pipeline {
  a::ModuleGraph graph;
  a::SemaResult semaResult;
  a::ir::Program program;
};

bool buildProgram(const std::string& source, Pipeline& pipeline) {
  a::DiagnosticSink sink;
  a::ModuleLoaderOptions options;
  a::ModuleLoader loader(std::move(options), sink);
  pipeline.graph = loader.load("test.a", source);
  if (sink.hasErrors()) return false;
  a::Sema sema(pipeline.graph, sink);
  pipeline.semaResult = sema.analyze();
  if (sink.hasErrors()) return false;
  pipeline.program = a::ir::lowerProgram(pipeline.graph, pipeline.semaResult);
  return true;
}

std::string functionByName(const a::ir::Program& program, const std::string& needle) {
  for (const auto& function : program.functions) {
    if (contains(function.name, needle)) return a::ir::printFunction(function);
  }
  return {};
}

void testLowering() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn add(a: int, b: int) -> int:
    return a + b

total = 0
i = 0
while i < 3:
    total = total + i
    i = i + 1
print(total)
print(add(1, 2))
)",
                                pipeline);
  check(ok, "lowering: program analyzes");
  if (!ok) return;

  const std::string text = a::ir::printProgram(pipeline.program);
  check(contains(text, "fn @"), "lowering: functions are printed");
  check(contains(text, "param a"), "lowering: parameters become Param instructions");
  check(contains(text, "phi [%"), "lowering: loop headers contain phi nodes");
  check(contains(text, "branch %"), "lowering: conditional flow uses branches");
  check(!functionByName(pipeline.program, "add").empty(), "lowering: function symbols exist");
}

void testConstantFolding() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn constants() -> int:
    x = 2 + 3 * 4
    return x

print(constants())
)",
                                pipeline);
  check(ok, "fold: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::constantFold(pipeline.program);
  check(stats.constantsFolded >= 2, "fold: binary expressions are folded");
  const std::string text = functionByName(pipeline.program, "constants");
  check(contains(text, "const 14"), "fold: 2 + 3 * 4 becomes const 14");
}

void testDeadCodeElimination() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn unused(x: int) -> int:
    y = x * 2
    return x

print(unused(3))
)",
                                pipeline);
  check(ok, "dce: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::deadCodeElimination(pipeline.program);
  check(stats.deadInstructions >= 1, "dce: unused pure instruction is removed");
  const std::string text = functionByName(pipeline.program, "unused");
  check(!contains(text, "* %0"), "dce: dead multiplication no longer printed");
}

void testInlining() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn add(a: int, b: int) -> int:
    return a + b

print(add(1, 2))
)",
                                pipeline);
  check(ok, "inline: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::inlineFunctions(pipeline.program);
  check(stats.functionsInlined >= 1, "inline: single-block function is inlined");
  const std::string main = functionByName(pipeline.program, "main");
  check(!contains(main, "call add"), "inline: call site is replaced");
}

void testLoopInvariantMotion() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
total = 0
i = 0
while i < 3:
    total = total + i * 7
    i = i + 1
print(total)
)",
                                pipeline);
  check(ok, "licm: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::loopInvariantCodeMotion(pipeline.program);
  check(stats.loopsHoisted >= 1, "licm: loop-invariant constant is hoisted");
  const std::string main = functionByName(pipeline.program, "main");
  check(contains(main, "const 7"), "licm: hoisted constant remains in the preheader");
}

void testStrengthReduction() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn shiftme(x: int) -> int:
    return x * 4
print(shiftme(3))
)",
                                pipeline);
  check(ok, "strength-reduction: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::algebraicSimplify(pipeline.program);
  check(stats.operationsSimplified >= 1, "strength-reduction: multiply simplified to shift");
  const std::string fn = functionByName(pipeline.program, "shiftme");
  check(contains(fn, "<<"), "strength-reduction: x * 4 becomes x << 2");
  check(contains(fn, "const 2"), "strength-reduction: shift amount is 2");
  check(!contains(fn, "*"), "strength-reduction: original multiply is gone");
}

void testCommonSubexpressionElimination() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn dup(x: int) -> int:
    a = x + 1
    b = x + 1
    return a + b
print(dup(3))
)",
                                pipeline);
  check(ok, "cse: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::commonSubexpressionElimination(pipeline.program);
  check(stats.subexpressionsEliminated >= 1, "cse: duplicate x + 1 eliminated");
  const std::string fn = functionByName(pipeline.program, "dup");
  check(contains(fn, "copy"), "cse: duplicate becomes a copy");
}

void testBranchFolding() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn f() -> int:
    if true:
        return 10
    return 20
print(f())
)",
                                pipeline);
  check(ok, "branch-fold: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::branchFolding(pipeline.program);
  check(stats.branchesFolded >= 1, "branch-fold: branch on constant folded to jump");
}

void testDeadStoreElimination() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn store_me() -> int:
    xs = [0, 0]
    xs[0] = 1
    xs[0] = 2
    return xs[0]
print(store_me())
)",
                                pipeline);
  check(ok, "dse: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::deadStoreElimination(pipeline.program);
  check(stats.deadStoresEliminated >= 1, "dse: overwritten store removed");
  const std::string fn = functionByName(pipeline.program, "store_me");
  check(contains(fn, "store_index"), "dse: surviving store still present");
}

void testOptLevels() {
  // -O0 is optnone: nothing inlined or folded yet.
  Pipeline off;
  const bool offOk = buildProgram(R"(
fn add(a: int, b: int) -> int:
    return a + b
print(add(1, 2))
)",
                                    off);
  check(offOk, "opt-level: program analyzes");
  if (!offOk) return;
  a::ir::runOptPipeline(off.program, a::ir::OptLevel::O0);
  check(contains(functionByName(off.program, "main"), "call add"),
        "opt-level: O0 leaves calls untouched");

  // parseOptLevel mirrors clang spellings.
  a::ir::OptLevel level = a::ir::OptLevel::O0;
  check(a::ir::parseOptLevel("-O3", level) && level == a::ir::OptLevel::O3,
        "opt-level: -O3 parses");
  check(a::ir::parseOptLevel("-Os", level) && level == a::ir::OptLevel::Os,
        "opt-level: -Os parses");
  check(!a::ir::parseOptLevel("-OX", level), "opt-level: bogus flag rejected");
}

void testRunAll() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn double(x: int) -> int:
    return x * 2

total = 0
i = 0
while i < 4:
    total = total + double(3)
    i = i + 1
print(total)
)",
                                pipeline);
  check(ok, "runAll: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::runAll(pipeline.program);
  check(stats.constantsFolded + stats.functionsInlined + stats.loopsHoisted > 0,
        "runAll: pipeline reports work");
  const std::string main = functionByName(pipeline.program, "main");
  check(!contains(main, "call double"), "runAll: helper call inlined");
}

void testLlvmStyleOps() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
xs = [[1, 2], [3, 4]]
v = xs[0][1]
f = f64(3)
g = i32(2.5)
)",
                                pipeline);
  check(ok, "llvm-ops: program analyzes");
  if (!ok) return;

  const std::string main = functionByName(pipeline.program, "main");
  check(contains(main, "getelementptr"), "llvm-ops: nested index lowers to getelementptr");
  check(contains(main, "cast sitofp"), "llvm-ops: int->float lowers to sitofp");
  check(contains(main, "cast fptosi") || contains(main, "cast fptoui"),
        "llvm-ops: float->int lowers to fptosi/fptoui");
}

void testInstCombine() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn pick(x: int) -> int:
    a = x & x
    b = x | x
    c = x ^ x
    return a - b + c
)",
                                pipeline);
  check(ok, "instcombine: program analyzes");
  if (!ok) return;

  a::ir::instCombine(pipeline.program);
  const std::string fn = functionByName(pipeline.program, "pick");
  check(!contains(fn, "&"), "instcombine: x & x folded to x");
  check(!contains(fn, "|"), "instcombine: x | x folded to x");
  check(contains(fn, "const 0"), "instcombine: x ^ x folded to 0");
}

void testConditionalConstantPropagation() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
x = 2 + 3
y = 0
if x == 5:
    y = 10
else:
    y = 20
print(y)
)",
                                pipeline);
  check(ok, "sccp: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::conditionalConstantPropagation(pipeline.program);
  check(stats.branchesFolded >= 1, "sccp: constant branch folded");
  const std::string main = functionByName(pipeline.program, "main");
  check(!contains(main, "branch"), "sccp: no conditional branch remains");
}

}  // namespace

int main() {
  testLowering();
  testConstantFolding();
  testDeadCodeElimination();
  testInlining();
  testLoopInvariantMotion();
  testLlvmStyleOps();
  testInstCombine();
  testConditionalConstantPropagation();
  testStrengthReduction();
  testCommonSubexpressionElimination();
  testBranchFolding();
  testDeadStoreElimination();
  testOptLevels();
  testRunAll();

  if (failures == 0) {
    std::cout << "ir tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
