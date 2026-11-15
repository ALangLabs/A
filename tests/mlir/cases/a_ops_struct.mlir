// Struct field access ops. a.struct.getfield/setfield parse and print, but
// a-lower-to-llvm has no lowering for them (!a.struct<"Name"> carries no
// field table), so this case runs with --no-emit-llvm-dialect and goldens
// the a-dialect text form.
module {
  a.func @main(%arg: !a.aval) -> !a.aval {
    %one = arith.constant 1 : i64
    %two = arith.constant 2 : i64
    %gf = a.struct.getfield %arg[%one] : !a.aval, i64 -> !a.aval
    %sf = a.struct.setfield %arg[%two] = %gf : !a.aval, i64, !a.aval -> !a.aval
    %ag = a.array.get %sf[%gf] : !a.aval, !a.aval -> !a.aval
    %as = a.array.set %sf[%gf] = %ag : !a.aval, !a.aval, !a.aval -> !a.aval
    a.return %as : !a.aval
  }
}
