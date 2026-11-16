# tests/mlir — `a mlir-opt` golden tests

Golden tests for the `a mlir-opt` subcommand (MLIR dialect lowering pipeline).

## Running

```sh
tests/mlir/run_tests.sh [path-to-a-binary]
```

Binary resolution: `$1`, else `$A_MLIR_OPT`, else `<repo>/build/a`.
Run from anywhere; the script resolves the repo root itself.

Output: one line per case (`FAIL name` with diffs on mismatch, `SKIP name
(reason)` for skipped cases), then a summary `passed: N, failed: M,
skipped: K`. Exit status is 1 when any case fails.

## Case format

All files live flat in `cases/`. One case per input file:

| file | meaning |
|------|---------|
| `name.mlir` | MLIR text input (the `a` dialect, `llvm` dialect, `arith`, `func`, ...) |
| `name.ambc` | AMBC bytecode input (produced by `a mlir-opt --emit-bytecode`) |
| `name.args` | extra CLI args for `a mlir-opt`, whitespace separated (e.g. `cse` or `llvm.func(cse)`). Multiple pass names may be given as separate words — they are comma-joined, so `cse canonicalize` runs pipeline `cse,canonicalize`. |
| `name.out` | expected stdout, exact `diff -u` match |
| `name.err` | expected stderr substrings, one per line (`grep -F` fixed-string match; every listed line must appear) |
| `name.exit` | expected numeric exit code (default `0`) |
| `name.skip` | skip the case; first line is the reason printed in the summary |
| `name.normalize.sed` | `sed -E` script applied to stdout before diffing (strip `loc(...)`, `; ModuleID`, etc.) |

Every listed sidecar is optional; a case with only `name.mlir` runs
`a mlir-opt name.mlir` and only checks that it exits 0.

The invocation is always `a mlir-opt <input> <args>` — pass arguments go
through the normal pass-argument join in the driver, so both single pipeline
strings (`llvm.func(cse)`) and comma lists (`cse,canonicalize`) work.

## Current cases

- `a_ops_roundtrip.mlir` — `a` dialect ops (a.func/a.const/a.alloca/a.load/
  a.store/a.gep/a.addressof/a.array.get/a.array.set/a.runtime.call/
  a.fib.*/a.call/a.for/a.while/a.condition/a.yield/a.return) through the
  default pipeline; golden is the lowered `llvm` dialect MLIR text.
- `a_ops_struct.mlir` — `a.struct.getfield`/`a.struct.setfield` under
  `--no-emit-llvm-dialect` (these ops are not yet lowered to llvm dialect).
- `typed_ops.mlir` — `a.to_aval` boxing for i64/f64/i1 scalars.
- `typed_unbox.mlir` — `a.to_aval`/`a.from_aval` round-trips under
  `--no-emit-llvm-dialect` (raw `from_aval` results cannot feed other `a`
  ops once types are rewritten to i64).
- `llvm_cse.mlir` — pure `llvm.func` module, `cse` pass dedups repeated ops.
- `default_pipeline.mlir` — minimal `a.func` module, no args: runs the full
  `buildAOptPipeline` (canonicalize/cse/mem2reg/sroa + a-lower-typed +
  a-lower-to-llvm).
- `pass_args.mlir` / `pass_args_multi.mlir` — `.args` plumbing: single quoted
  pipeline vs. multiple pass words.
- `ambc_roundtrip.ambc` — AMBC bytecode input; golden is the printed module.
- `ambc_bad.ambc` — garbage bytes; expects exit 1 and the compat-magic error.

## Adding a case

1. Write `cases/<name>.mlir` (or generate `cases/<name>.ambc` — see below).
2. Add sidecars as needed (`.args`, `.out`, `.err`, `.exit`, `.skip`,
   `.normalize.sed`).
3. Generate goldens from real tool output — never hand-write them:

```sh
./build/a mlir-opt tests/mlir/cases/<name>.mlir <args> \
  > tests/mlir/cases/<name>.out 2> tests/mlir/cases/<name>.err
# expected exit code:
echo $? > /dev/null   # then write tests/mlir/cases/<name>.exit if non-zero
```

For bytecode cases:

```sh
./build/a mlir-opt tests/mlir/cases/<src>.mlir --emit-bytecode \
  -o tests/mlir/cases/<name>.ambc
```

4. Run `tests/mlir/run_tests.sh build/a` and confirm the case passes.

## Notes

- `--emit-llvmir` only works on modules already in the `llvm` dialect (it
  skips the pass pipeline); `a`-dialect input must go through the default
  pipeline, whose golden is the lowered MLIR text, not LLVM IR.
- Ops the pipeline dead-code-eliminates (pure `a.to_aval`/`a.from_aval`,
  unused loads) must have their results consumed by side-effecting ops
  (`a.store`, `a.runtime.call`, `a.fib.*`, `a.return`) to appear in goldens.
- `arith.constant ... : i1` literals must be `0`/`1` in input (they print as
  `true`/`false`).
