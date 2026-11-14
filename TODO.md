# TODO

- Native only: the LLVM backend (`frontend/llvm/llvm_emit.cpp`) is the sole
  execution path for `a run`, `a build`, `a test`, and `a repl`.
- No bytecode/VM backend is planned; the former `compiler/astra/frontend/vm/`
  tree was removed.
