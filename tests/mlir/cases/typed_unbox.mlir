// a.from_aval unboxes !a.aval into typed scalars. The lowering
// (a-lower-typed) currently only supports from_aval results consumed inside
// the typed subgraph; any surviving boxed-ABI consumer fails verification
// after the pass rewrites result types to raw scalars, and merge-block
// results crash a-lower-to-llvm. This case therefore runs with
// --no-emit-llvm-dialect and goldens the a-dialect text form.
module {
  a.func @unbox_i64(%arg: !a.aval) -> !a.aval {
    %c = arith.constant 1 : i64
    %b0 = a.to_aval %c : i64 -> !a.aval
    %x = a.from_aval %b0 : !a.aval -> i64
    %y = arith.addi %x, %x : i64
    %b = a.to_aval %y : i64 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %arg) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
  a.func @unbox_f64(%arg: !a.aval) -> !a.aval {
    %c = arith.constant 0.5 : f64
    %b0 = a.to_aval %c : f64 -> !a.aval
    %x = a.from_aval %b0 : !a.aval -> f64
    %y = arith.addf %x, %x : f64
    %b = a.to_aval %y : f64 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %arg) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
  a.func @unbox_i1(%arg: !a.aval) -> !a.aval {
    %c = arith.constant 1 : i1
    %b0 = a.to_aval %c : i1 -> !a.aval
    %x = a.from_aval %b0 : !a.aval -> i1
    %y = arith.xori %x, %x : i1
    %b = a.to_aval %y : i1 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %arg) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
}
