# Typed-scalar ABI vs the aval-only C runtime — research spec

Phase-2 MLIR work (`docs/mlir-typed-values-design.md`) unboxes
Int/Float/Bool/Char/Vector to native `iN`/`fN`/`i1`/`vector` in the `a`
dialect, bridged to the boxed world by `a.to_aval`/`a.from_aval`. The C
runtime (`src/astra/runtime/*.c`, ~22k lines) only understands `aval`
(`uint64_t`, `a_runtime.h:36`). This spec answers: what breaks in GC, what
the call boundary must look like, exact box/unbox encodings, fiber
impact, and top migration hazards. Refs are `path:line` vs working tree.

## 0. Aval encoding ground truth (two layers, must not conflate)

- **MLIR text emitter**: `kAvalTag = -281474976710656` =
  `0xFFFF000000000000`, `kInlineLim = 1<<46` (`mlir_emit.cpp:33-34`).
  `avalIntEncode(v)` (`mlir_emit.cpp:36-40`) = `(v<<1)|1` masked to 48
  bits OR'd with `kAvalTag`. Range gate `v >= -kInlineLim && v < kInlineLim`
  (`mlir_emit.cpp:277`); out-of-range -> `a_i64n` (`mlir_emit.cpp:278`).
- **C header**: `A_TAG = 0xFFFF000000000000`, `A_PAYLOAD = v & 0x0000FFFFFFFFFFFF`,
  `A_IS_TAGGED = (v & A_TAG_MASK)==A_TAG` (`a_runtime.h:39-42`). `a_mkint`
  (`a_runtime.h:56-59`) is identical to `avalIntEncode`; `a_is_int` =
  tagged && bit0 (`a_runtime.h:60`); `a_int_val` shifts payload>>1 and
  sign-extends from bit 46 (`a_runtime.h:61-66`).
- **Full-width ints**: |v| >= 2^46 box as `T_INT64` heap objects via
  `mkint_full` (`core_util.c:417-427`, bump arena `core_util.c:376-415`).
  `a_i64n` -> `mkint_full` (`a_runtime.c:316`); `a_int_full`/`int_val_full`
  accept both forms (`a_runtime.c:223`, `core_util.c:351-357`). Emitter
  decl `a_i64n` is `(i64)->i64` taking a RAW i64 (`mlir_emit.cpp:760`).
- **Doubles**: raw bit pattern, no tag (`a_mkdouble`/`a_double_val`,
  `a_runtime.h:67-77`); NaN canonicalized to `0x7FF8000000000000`
  (`core_util.c:430-436`, `mlir_emit.cpp:280-285`).
- **Small constants**: `A_NONE=|0x2`, `A_FALSE=|0x4`, `A_TRUE=|0x6`,
  `A_STDOUT=|0x8`, `A_STDERR=|0xA` (`a_runtime.h:46-53`); `A_MISSING` =
  bare `kAvalTag` (`mlir_emit.cpp:912`).

Consequence: `a.to_aval` for `i64` MUST range-check and pick inline vs
`a_i64n` — exactly mirroring `avalInt`/`mkint_full`. The 46-bit inline
limit is load-bearing for every `a_is_int`/`a_int_val` fast path.

## 1. GC safety under unboxed scalars

### How GC works today (verified)

- Conservative mark-sweep over EVERY xmalloc/xrealloc block (`gc.c:1-55`):
  each 8-byte word of each marked block is tested as a pointer; hits mark
  the containing block whose words are then scanned (worklist fixpoint,
  `gc.c:14-17`, `mark_scan_block` `gc.c:915`). Buffer contents are scanned,
  so avals in C temporaries (argv, map results) stay alive (`gc.c:15-17`).
- Roots: C stack `[sp, top)` (`gc.c:1046-1074`, bounds `gc.c:941-1032`),
  callee-saved regs via setjmp (`gc.c:25`), root-provider callbacks
  (`gc.c:622-641`), emitted `__a_gc_root_mark` table (`gc.c:1089-1103`).
- Fiber stacks are roots: `fib_gc_roots` marks args + scans suspended
  fiber stacks via `a_gc_scan_region` (`async.c:241-261`, `gc.c:1038-1044`).
- `a_pin_slot` is a NO-OP (`core_util.c:171`): it only forces the alloca
  address-taken so LLVM won't promote it and hide the aval from the
  conservative stack scan (`core_util.c:167-170`, `mlir_emit.cpp:320-322`).
  Used on iterator/parfor slots (`mlir_emit.cpp:1500,2309-2317,2462`).
- **Regions (young gen) are NOT conservative**: `a_frame_push/pop` bracket
  every A fn; unpromoted objects are bulk-freed at `a_frame_pop`
  (`region.c:86-125`). Promotion happens only via write barrier
  `a_region_store(container, aval child)` at heap-store sites
  (`region.c:293-297`) and transitive `a_promote` over aval fields
  (`region.c:272-286`).

### (a) Aggregate pointer fields stay boxed aval — verified safe

Struct fields live in an `ADict` (`T_STRUCT`, `gc.c:1139-1144`;
`a_getattr` reads `d->pairs[i].v`, `methods.c:100-108`); list/tuple/set
items in `AArr.items`/`iitems` (`gc.c:1133-1138`); dict pairs/slots;
`AEnv.slots`; `AFnon.env`. All are registered GC blocks whose words are
scanned. So a struct's pointer fields remain findable whenever the struct
object is reachable — independent of how the *local* holding the struct
aval is represented. M3 unboxed `{i64,f64,...}` struct values keep aval
pointer fields inside the alloca; stack-resident allocas are scanned, and
SSA-promoted fields are seen via spill slots (calls clobber regs, so
LLVM spills them — same tolerance Boehm GC has). Residual risk is the
region barrier, not the mark-sweep (see hazard H1).

### (b) Unboxed i64 locals aliasing pointer bits

- False positives: arbitrary i64/f64 bit patterns on the scanned stack
  will mark random blocks — retention, not corruption. Today locals are
  avals so junk i64s rarely appear; after unboxing they flood the scan.
  No cheap fix; accept and measure. (Irony: fully promoting scalar
  allocas to SSA *reduces* scanned junk vs keeping them memory-resident.)
- Missed pointers: none from unboxing itself — pointers exist only as
  aval fields in heap objects or as `!a.ptr`/`!llvm.ptr`; emitted code
  never produces raw object pointers into the graph (`unsafe` module
  aside, which uses `a_gc_mark_ptr`, `a_runtime_internal.h:867`).
- Real hazard is emitter bugs producing malformed avals: `a_is_ptr`
  (`a_runtime.h:78-80`) accepts any tagged, 16-aligned nonzero payload —
  a wrong `to_aval` can point GC at a non-object block.
- `a_pin_slot` still required for allocas holding AVALS across calls
  (iterator item slots written by `a_iter_next(it,&slot)`); unneeded for
  pure-scalar allocas.

## 2. Runtime call boundary

All `a_*` C fns take/return `aval` (`a_runtime.h:226-600`); A fns use
`aval f(aval *argv)`, argv[0]=env (`a_runtime.h:18-22`). The `a` dialect
keeps `a.runtime.call` signature `"(i64,...)->i64"` and wraps operands
in `a.to_aval` / results in `a.from_aval` (design §3, :149-152).

The emitter declares 65 runtime sigs (`mlir_emit.cpp:741-807`). Classes:

- **Group A — scalar ops, native-arg candidates (~24)**: `a_add_slow`..
  `a_pow`, `a_eq_slow`..`a_ge_slow`, `a_band_slow`..`a_shr_slow`,
  `a_neg_slow`,`a_bnot_slow`,`a_not`,`a_truthy_raw_slow`. Hottest ops in
  the sample (86 `a_add_slow`, 54 `a_eq_slow`, 30 `a_lt_slow` call sites —
  `mlir-emitter-inventory.md:225-230`). **Recommendation: do NOT add
  native-arg variants — emit `arith.*` dialect ops for proven scalars and
  keep `*_slow` only as the mixed-type fallback.** `a_*_fast` exports
  exist (`a_runtime.h:568-589`, `fastpath.c`) but the MLIR emitter
  deliberately uses only `_slow` (`mlir-emitter-inventory.md:120-122`).
- **Group B — one side boxed (~19)**: `a_str`,`a_repr`,`a_len`,`a_int`,
  `a_float`,`a_bool`,`a_type_name`,`a_int_full`,`a_i64n`,`a_str_new`,
  `a_char_new`,`a_some_new`,`a_unwrap`,`a_try_result`,`a_promote`,
  `a_panic_msg`,`a_join_len`,`a_match_fail`,`a_is_option`,`a_is_propagate`.
  Keep aval ABI; box/unbox the scalar side.
- **Group C — must stay aval (~42)**: all container/iter/call/GC/frame/
  fib/IO fns (`a_list_*`,`a_dict_*`,`a_getitem`,`a_setitem`,`a_method`,
  `a_call`,`a_bi_call`,`a_getattr`,`a_setattr`,`a_fnon_new`,`a_env_*`,
  `a_iter_*`,`a_fib_*`,`a_gc_*`,`a_frame_*`,`a_pin_slot`,`a_struct_new`,
  `a_test_main`, `a_print*`, ...). Operate on heap objects with aval
  fields; scalar args still need boxing into argv arrays.

Hotness: Group A ≈350 call sites vs Group C ≈800+ in the 1.5 MB ndarray
sample; Group C dominates count but each call is already mandatory — the
only saving is cheaper scalar arg boxing (3-4 LLVM ops inline-int, or an
`a_i64n` call + heap alloc for |v|>=2^46).

## 3. `a.to_aval` / `a.from_aval` exact semantics

### `a.to_aval : scalar -> !a.aval` (result i64)

| src | lowering |
|---|---|
| `iN` signed, \|v\|<2^46 | `trunc/sext to i64` -> `(v<<1)|1` (`shl`+`or i64 1`) -> `or 0xFFFF000000000000`. No call. |
| `i64`/`u64` full | wrap-safe range check `icmp ult (v + 2^46), 2^47` -> `cond_br`: in-range inline encode; else `llvm.call @a_i64n(v)` (raw i64 arg). |
| `fN` | `fpext/fptrunc to f64` -> `bitcast f64 to i64` -> NaN check -> canonical `0x7FF8000000000000` if NaN (mirror `avalDbl`/`mkdouble`). |
| `i1` | `zext` -> `select` -> `kAvalTag|0x6` (A_TRUE) / `|0x4` (A_FALSE). |
| `i32` char | PROBLEM: `a_char_new` needs a UTF-8 `ptr,len` (`mlir_emit.cpp:762`); char->i32 unboxing needs an inline UTF-8 encoder or a runtime helper. Keep Char boxed in M1. |
| `!a.aval` | canonicalizer no-op (design :127). |
| `vector<NxT>` | no aval repr; stays `!a.aval`/deferred (design M-later). |

### `a.from_aval : !a.aval -> scalar` — two distinct ops

**`a.from_aval` (unchecked ABI unpack)** — only where Sema statically
guarantees the type (design verifier rule, :131-135):

- `-> i64`/`iN`: aval may be inline int OR T_INT64 box. Decode:
  1. extract payload `p = v & 0x0000FFFFFFFFFFFF`; if `(v>>48)==0xFFFF && (p&1)` -> inline: `q = p>>1`, sign-extend from bit46 (`(q<<17) ashr 17`) -> `trunc/sext to iN`.
  2. else -> `llvm.call @a_int_full(v) -> i64` (raw int64 out, `a_runtime.h:545`, panics on non-int `core_util.c:351-357`) -> `trunc to iN`.
  The `a_int_full` fallback is REQUIRED — inline-only decode silently
  misreads T_INT64 boxes (any |v|>=2^46, e.g. the literal `2^50` emitted
  via `a_i64n` at `mlir_emit.cpp:278`). A cheaper `a.from_aval_inline`
  variant is only legal for provably-inline producers.
- `-> fN`: `is_tagged = icmp eq (v & 0xFFFF000000000000), 0xFFFF000000000000`;
  untagged -> `bitcast i64 to double`; tagged -> `call @a_float(v)` (aval
  in/out) then bitcast (a_float always yields a double aval).
- `-> i1`: legal only where Sema proves bool (A_TRUE/A_FALSE only):
  `icmp eq v, kAvalTag|0x6`. A_NONE/other must NOT reach this op —
  truthiness contexts use `a_truthy_raw` semantics, not from_aval.

**`a.unbox<T>` (checked mid-expression cast)**: separate op calling a
runtime type check; for int needs `expect_int64`-style wording
(`core_util.c:359-363`), NOT `a_int_full`'s "internal:" panic.

**CRITICAL detail**: small ints use the INLINE encoding `(v<<1)|1` in
the low 48 bits OR'd with `kAvalTag`, gated at |v| < 2^46
(`avalIntEncode` `mlir_emit.cpp:36-40`, gate `:277`). There is no
separate small-int tag; payload bit0 is the int discriminator
(`a_is_int` `a_runtime.h:60`). `from_aval` MUST handle both inline and
T_INT64 forms — assuming "always full tag" or "always inline" is wrong.

## 4. Fibers / async

`a_fib_spawn_handle(aval fnval, uint64_t n, aval *args)`
(`async.c:804-815`) takes an aval fnval + aval argv; `fib_trampoline`
(`async.c:328-349`) calls `a_call(f->fnval, f->nargs, f->args+1)`.
fnvals are `AFnon` (T_FNON) whose `entry` is a raw u64 fn pointer with
the boxed `aval(*)(aval*)` signature (`a_runtime.h:18-22`,
`value_closure.c:101`).

With typed fns (design M4 mode b, :157-162):

- A typed `llvm.func @f(i64,f64)->i1` cannot be wrapped by
  `a_fnon_new` directly — wrong signature. Needs a per-fn **trampoline**
  `__a_tramp_<cname>(aval *argv)->aval` that `from_aval`s each param,
  calls the typed fn, `to_aval`s the result; `a_fnon_new(ptrtoint
  (@__a_tramp), env, name)` then works unchanged. This IS the "address-
  taken stays boxed" gate — every escaping fn gets a trampoline.
- `a_fib_spawn_handle`/`a_call`/`a_bi_call`/`a_method` keep aval ABI —
  no runtime change IF every fn-as-value goes through a trampoline.
- Parfor fns (`__a_pf_<n>`, `emitParforFn` `mlir_emit.cpp:2394-2416`)
  take `(env, loopvar)` via argv; if loopvar unboxes to i64 the fn
  signature types -> needs trampoline; spawn-side `argArr` packing
  (`mlir_emit.cpp:2317,2340-2343`) must `to_aval` the loop var once.
- Fiber GC safe: `fib_gc_roots` scans suspended stacks conservatively
  (`async.c:245-259`); `f->args` is raw `aval*` memcpy'd
  (`async.c:317-326`) — stays aval.

## 5. Top-5 migration hazards (likelihood x impact)

1. **Region write barrier bypass (highest risk).** `a_region_store`
   (`region.c:293-297`) fires only on aval-typed stores through runtime
   calls; unboxed aggregate/scalar stores bypass it. A young object
   stored into an unboxed container then boxed later, or returned via a
   typed fn whose result escapes without `a_promote`, can be
   region-swept while still referenced. Mark-sweep is the safety net but
   regions free eagerly at `a_frame_pop` (`region.c:107-125`). Every new
   store path in typed code needs barrier analysis — this is the least
   understood surface.
2. **`from_aval` decode of T_INT64 boxes.** Inline-only decode
   `(v>>1)` sign-extend yields garbage for |v|>=2^46 avals (pointer
   bits), producing wrong values or GC marking random blocks. Must keep
   the `a_int_full` fallback (§3). High likelihood because inline decode
   is the tempting fast path.
3. **Boundary double-boxing without fold.** Each runtime call re-boxes
   scalar args (`argvPack` `mlir_emit.cpp:389-394`) and unboxes results;
   without canonicalizer folding of `from_aval(to_aval(x))` pairs
   (does not exist yet, design :127) numeric kernels pay 2 ops per call,
   eroding the arith win.
4. **Mixed-signature decls.** Not all 65 sigs are `(i64..)->i64` aval:
   `a_range_bound` header is `(aval,int64_t)->int64_t` (`a_runtime.h:490`)
   vs decl `(i64,i64)->i64` (`mlir_emit.cpp:776`); `a_is_option`/
   `a_is_propagate` return i32; `a_printv`/`a_frame_push` return void;
   `a_test_main` returns i32 (`a_runtime.h:544`). Typed wrappers must be
   audited per-symbol or they miscompile.
5. **f64->aval NaN/tag collision.** `a_mkdouble` returns raw bits; only
   negative-NaN-space patterns alias `A_TAG` (`a_runtime.h:5-11`), and
   producers must canonicalize (`mkdouble`/`avalDbl` do). Any typed
   `to_aval` f64 site that skips the NaN check emits an aval `a_is_ptr`
   treats as a pointer -> GC corruption. Audit every C double producer
   (`value_arith.c`, `builtins_math.c`, `fastpath.c`) for canonicalization.

Secondary: `i1` decode must not map A_NONE->false outside proven-bool
contexts; Char->i32 needs UTF-8 encode at `to_aval`; `argv[0]=env`
convention (`mlir_emit.cpp:400`) must be preserved by trampolines;
`__a_gc_root_mark` covers only globals (`mlir_emit.cpp:862-874`) so
typed globals need per-symbol type info; `vector<NxT>` has no aval repr.

## Appendix: key refs

- encode/decode: `a_runtime.h:39-80`, `core_util.c:351-436`,
  `mlir_emit.cpp:33-40,234-285`
- GC: `gc.c:1-55,596-641,901-934,1038-1103,1133-1160`
- regions/barrier: `region.c:86-125,272-297`, `a_runtime_internal.h:911-924`
- pin_slot: `core_util.c:167-171`; uses `mlir_emit.cpp:320-322,1500,2309-2317`
- fibers: `async.c:241-268,317-375,804-840`; fn ABI `a_runtime.h:18-22`
- decls/calls/fn ABI: `mlir_emit.cpp:741-807,331-421,877-933`
- Sema types: `sema.hpp:77,84`, `sema_expr.cpp:8`, `types.hpp:63-96`
