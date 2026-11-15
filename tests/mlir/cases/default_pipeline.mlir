// Minimal a.func module run through the default `a` pipeline
// (canonicalizer, CSE, mem2reg/SROA, a-lower-typed, a-lower-to-llvm).
module {
  a.func @main(%arg: !a.aval) -> !a.aval {
    %c = a.const int {ivalue = 42 : i64} -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%arg, %c) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
}
