// llvm-dialect CSE: the two adds and two muls are duplicates; `cse` must
// fold each pair to a single op.
module {
  llvm.func @cse_demo(%a: i64, %b: i64) -> i64 {
    %s1 = llvm.add %a, %b : i64
    %s2 = llvm.add %a, %b : i64
    %m1 = llvm.mul %s1, %s2 : i64
    %m2 = llvm.mul %s1, %s2 : i64
    %d = llvm.sub %m1, %m2 : i64
    llvm.return %d : i64
  }
}
