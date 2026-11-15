// a_ops_roundtrip.mlir -- exercises every parseable `a`-dialect op form.
// NOTE: a.func, a.call, a.const, a.return, a.runtime.call, a.fib.*, a.for,
// a.while, a.condition have no custom assembly form in this build of the
// dialect (see mlir_emit_ad.cpp DIALECT STATUS) and cannot appear in MLIR
// text; they are covered indirectly via --emit-bytecode roundtrips.
module {
  llvm.func @roundtrip(%arg0: i64) -> i64 {
    %c0 = arith.constant 0 : i64
    %c1 = arith.constant 1 : i64
    %c2 = arith.constant 2 : i64
    %f64v = arith.constant 3.5 : f64
    %cmp = arith.cmpi slt, %c0, %c1 : i64

    // boxing boundary (to_aval / from_aval)
    %a_i = a.to_aval %c0 : i64 -> !a.aval
    %a_f = a.to_aval %f64v : f64 -> !a.aval
    %a_b = a.to_aval %cmp : i1 -> !a.aval
    %u_i = a.from_aval %a_i : !a.aval -> i64
    %u_f = a.from_aval %a_f : !a.aval -> f64
    %u_b = a.from_aval %a_b : !a.aval -> i1

    // stack memory
    %slot = a.alloca %c1 x !a.aval : i64 -> !a.ptr
    a.store %a_i, %slot : !a.aval, !a.ptr
    %ld = a.load %slot : !a.ptr -> !a.aval
    %ge = a.gep %slot[%c0] : (!a.ptr, i64) -> !a.ptr
    %ad = a.addressof @g_str : !a.ptr

    // aggregates
    %gf = a.struct.getfield %ld[%c1] : !a.struct<"S"> : !a.aval, i64 -> !a.aval
    %sf = a.struct.setfield %ld[%c1] = %a_i : !a.struct<"S"> : !a.aval, i64, !a.aval -> !a.aval
    %ag = a.array.get %ld[%a_i] : !a.array<4> : !a.aval, !a.aval -> !a.aval
    %as = a.array.set %ld[%a_i] = %a_i : !a.array<4> : !a.aval, !a.aval, !a.aval -> !a.aval

    llvm.return %u_i : i64
  }
}
