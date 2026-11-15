// Phase-2 typed-scalar boxing (docs/mlir-typed-values-design.md M1).
// a-lower-to-llvm only supports i64/pointer a.func parameters, so the f64/i1
// scalars are materialized inside the body from the i64 parameter; a.to_aval
// boxes them into the aval ABI for the runtime call.
module {
  a.func @box_i64(%x: i64) -> !a.aval {
    %b = a.to_aval %x : i64 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %b) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
  a.func @box_f64(%x: i64) -> !a.aval {
    %f = arith.sitofp %x : i64 to f64
    %b = a.to_aval %f : f64 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %b) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
  a.func @box_i1(%x: i64, %y: i64) -> !a.aval {
    %c = arith.cmpi eq, %x, %y : i64
    %b = a.to_aval %c : i1 -> !a.aval
    %rc = a.runtime.call "a_add_slow"(%b, %b) : (!a.aval, !a.aval) -> !a.aval
    a.return %rc : !a.aval
  }
}
