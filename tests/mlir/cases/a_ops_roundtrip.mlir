module {
  llvm.mlir.global private constant @g_str("hello") : !llvm.array<5 x i8>

  a.func @callee(%x: !a.aval) -> !a.aval {
    %c = a.const int {ivalue = 7 : i64} -> !a.aval
    a.return %c : !a.aval
  }

  a.func @main(%arg: !a.aval) -> !a.aval {
    // literals: every a.const kind
    %ci = a.const int {ivalue = 5 : i64} -> !a.aval
    %cf = a.const float {fvalue = 1.5 : f64} -> !a.aval
    %cb = a.const bool {ivalue = 1 : i64} -> !a.aval
    %cs = a.const str {str = "hello"} -> !a.aval
    %cn = a.const none -> !a.aval

    // stack memory
    %one = arith.constant 1 : i64
    %zero = arith.constant 0 : i64
    %slot = a.alloca %one x !a.aval : i64 -> !a.ptr
    a.store %arg, %slot : !a.aval, !a.ptr
    %ld = a.load %slot : !a.ptr -> !a.aval
    %ge = a.gep %slot[%zero] : (!a.ptr, i64) -> !a.ptr
    %ld2 = a.load %ge : !a.ptr -> !a.aval
    %ad = a.addressof @g_str : !a.ptr
    %ld3 = a.load %ad : !a.ptr -> !a.aval

    // aggregates (array element access lowers to a_getitem/a_setitem)
    %ag = a.array.get %ld[%arg] : !a.aval, !a.aval -> !a.aval
    %as = a.array.set %ld[%arg] = %ag : !a.aval, !a.aval, !a.aval -> !a.aval

    // runtime escape hatch + fibers + direct call
    %rc = a.runtime.call "a_add_slow"(%as, %as) : (!a.aval, !a.aval) -> !a.aval
    %h = a.fib.spawn %rc, %one, %as : !a.aval, i64, !a.aval -> i64
    %jr = a.fib.join %h : i64 -> !a.aval
    %fm = a.fib.main %zero, %one, %slot, %as : i64, i64, !a.ptr, !a.aval -> !a.aval
    %call = a.call @callee(%as) : (!a.aval) -> !a.aval

    // counted loop with a.yield
    %lo = arith.constant 0 : index
    %hi = arith.constant 3 : index
    %st = arith.constant 1 : index
    %loop = a.for %lo to %hi step %st iter_args(%as) : index to index step index, !a.aval -> !a.aval {
    ^bb0(%iv: index, %acc: !a.aval):
      a.yield %acc : !a.aval
    }

    a.return %loop : !a.aval
  }
}
