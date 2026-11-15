// a.while parses (scf.while-style syntax) but currently fails
// RegionBranchOpInterface verification: getSuccessorRegions reports the
// `before` region successor without its block arguments, so any init operand
// count mismatches ("target successor needs 0"). Frozen as a negative case.
module {
  func.func @main(%arg0: i64) -> i64 {
    %z = arith.constant 0 : i64
    %a0 = a.to_aval %z : i64 -> !a.aval
    %r = a.while (%a0) : (!a.aval) -> !a.aval {
      ^bb0(%a2: !a.aval):
        %cond = arith.cmpi eq, %arg0, %z : i64
        a.condition(%cond) %a2 : !a.aval
    } do {
    ^bb0(%x2: !a.aval):
      a.yield %x2 : !a.aval
    }
    func.return %arg0 : i64
  }
}
