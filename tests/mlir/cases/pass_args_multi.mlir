// Pass-pipeline argument plumbing: .args selects the pipeline.
module {
  llvm.func @p(%a: i64, %b: i64) -> i64 {
    %s1 = llvm.add %a, %b : i64
    %s2 = llvm.add %a, %b : i64
    %d = llvm.sub %s1, %s2 : i64
    llvm.return %d : i64
  }
}
