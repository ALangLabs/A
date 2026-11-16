# MLIR emitter inventory and lowering-equivalence spec

Source of truth: `src/astra/frontend/mlir/mlir_emit.cpp` (3,086 lines, `emitMLIR` at line 3079).
Runtime ABI: `src/astra/runtime/a_runtime.h`. Existing `a` dialect: `src/astra/mlir/include/A/{ATypes,AOps}.td`, `src/astra/mlir/passes/ALowerToLLVM.cpp`.

Terminology: `aval` = boxed A value, a `uint64_t`. Tagged immediates:
`kAvalTag = -281474976710656` (mlir_emit.cpp:31), `kInlineLim = 1<<46` (:32).
`avalIntEncode(v)` (:34) = `(v<<1)|1` masked to 48 bits, OR'd with `kAvalTag`.
`A_NONE = kAvalTag|0x2`, `A_TRUE = kAvalTag|0x6`, `A_FALSE = kAvalTag|0x4`,
stdout=`kAvalTag|0x8`, stderr=`kAvalTag|0xA`, `A_MISSING = kAvalTag` bare (:2791).
`avalInt` (:234) emits an `llvm.mlir.constant` of the encoded i64 when in
inline range, else `a_i64n`. `avalDbl` (:238) bit-casts f64->i64 constant
(NaN -> `9221120237041090560`). `avalBool` (:244). Doubles/strings/objects are
heap pointers stored as tagged i64s; ALL values cross function boundaries as i64.

Emission order in `run()` (:69-86): `collectTopLevel` -> `emitModuleOpen`
(`module {`) -> `emitDecls` -> `emitGlobals` -> `resolveImpls` -> `emitFunctions`
-> `emitStructCtors` -> `emitMainBody` -> `emitGcRoots` -> `emitMain`/`emitTestMain`
-> `emitLambdas`. Output is a single `module { ... }` containing llvm.func
decls, globals, and every function definition. All allocas for a function are
hoisted into `fnAllocs_` and spliced by `flushFunction` (:918).

## 1. Construct inventory

| A construct | mlir_emit.cpp anchor | MLIR text pattern emitted | Runtime fn(s) |
|---|---|---|---|
| int literal (inline range) | :234 `avalInt` | `llvm.mlir.constant(-281474976710649 : i64) : i64` (encoded) | none |
| int literal (out of range) | :234 | `llvm.call @a_i64n(%c : i64) : (i64) -> i64` | `a_i64n` |
| float literal | :238 `avalDbl` | `llvm.mlir.constant(<bitcast i64> : i64) : i64` | none |
| bool / None | :244, consts | `llvm.mlir.constant(-281474976710650/-654 : i64)` | none |
| char literal | :1503-1544 | private `!llvm.array<N x i8>` global + `llvm.mlir.addressof` + `a_char_new(ptr,len)` | `a_char_new` |
| string literal | `evalStringLit` :2552, `stringLiteralAval` :1466 | private constant `!llvm.array<N x i8>` global built with `llvm.mlir.undef` + per-byte `llvm.mlir.constant(i8)` + `llvm.insertvalue` chain; expr parts via `a_str`+`a_str_concat` | `a_str_new`, `a_str`, `a_str_concat` |
| f-string | parts path inside `evalStringLit` | concat of `a_str(part)` values + `a_str_concat` | `a_str`, `a_str_concat` |
| list literal | :1654 | `a_list_new(0,null)` then `a_list_append(list, elem)` per elem | `a_list_new`, `a_list_append` |
| tuple literal | :1663 | `argvPack` into `allocaArr` stack array + `a_tuple_new(n, ptr)` | `a_tuple_new` |
| dict literal | :1671 | `a_dict_new()` + `a_dict_set(d,k,v)` per pair | `a_dict_new`, `a_dict_set` |
| set literal | :1679 | `a_set_new()` + `a_set_add(s,v)` per elem | `a_set_new`, `a_set_add` |
| range expr | :1686 | `a_range_new(lo, hi, step)` | `a_range_new` |
| slice spec | :1693 | 3-elem list via `allocaArr` + `a_list_new` | `a_list_new` |
| slice expr | :1706 | `a_slice(obj, lo, hi, step)` | `a_slice` |
| binary arith `+ - * / // % **` | `emitBinOp` :2627-2665 | `llvm.call @a_add_slow(a,b) : (i64,i64)->i64` etc. — NEVER inline `llvm.add` | `a_add_slow`,`a_sub_slow`,`a_mul_slow`,`a_div_slow`,`a_floordiv_slow`,`a_mod_slow`,`a_pow` |
| compare `== != < <= > >=` | :2627 | `llvm.call @a_eq_slow` / `a_ne_slow` / `a_lt_slow` / `a_le_slow` / `a_gt_slow` / `a_ge_slow` | same-named `_slow` fns |
| bitwise `& | ^ << >>` | :2627 | `a_band_slow`,`a_bor_slow`,`a_bxor_slow`,`a_shl_slow`,`a_shr_slow` | `_slow` variants |
| `in` / `not in` | :2627 | `a_contains(container, elem)`; `not in` wraps in `a_not` | `a_contains`, `a_not` |
| unary `- ~ ! not` | :2607 | `a_neg_slow`,`a_bnot_slow`,`a_unwrap` (`!`),`a_not` | those fns |
| `++`/`--` | `emitIncDec` :2571 | `a_add_slow`/`a_sub_slow` + `a_getitem`/`a_setitem`/`a_getattr`/`a_setattr` | mixed |
| `and`/`or`/`&&`/`||` | `logicExpr` :2704 | short-circuit via alloca result slot + `llvm.cond_br` | `a_truthy_raw_slow` per operand |
| `??` | `coalesceExpr` :2678 | None-check (`icmp` vs A_NONE const) + `a_unwrap` on rhs | `a_unwrap` |
| ternary `?:` | `ifExprResult` :2734 | alloca slot + `llvm.cond_br` arms storing i64 | none direct |
| if stmt | `emitIf` :1332 | linear `llvm.cond_br`/`llvm.br` chain, `^L<n>` labels, per-arm scope push/pop | `a_truthy_raw_slow` for cond |
| while / do-while / loop | :1368/:1390/:1447 | cond_br back-edges, break/continue label stacks | `a_truthy_raw_slow` |
| for-in | `emitFor` :1412 | iterator protocol: `a_iter_new(coll)`, pinned itemSlot alloca, loop `a_iter_next(it,&slot)` -> i64 truthy -> `icmp ne` -> `llvm.cond_br` | `a_iter_new`, `a_iter_next`, `a_pin_slot` |
| parfor `for x in range(...)` | `parforEligible` :2187, `emitForParfor` :2199, `emitParforFn` :2311 | eligible loops become `__a_pf_<n>` fn + closure env; spawn loop `a_fib_spawn_handle(fn,1,&arg)` collecting handles in list; join loop `a_fib_join_handle(h)` | `a_fib_spawn_handle`, `a_fib_join_handle`, `a_fnon_new`, `a_env_*` |
| let binding | :1148 `bindLocal` | `llvm.alloca` hoisted + `llvm.store` of aval | none |
| assign ident | `emitAssign` :1237, `storeBinding` :469 | local: `llvm.store`; global: `llvm.mlir.addressof @__a_g_*` + store; auto-vivify creates `__a_g_<modid>_<name>` global + GC root | `a_gc_mark_aval` (via root fn) |
| assign index `a[i]=v` | :1237 | `llvm.call @a_setitem(obj, idx, val)` | `a_setitem` |
| assign field `o.f=v` | :1237 | `llvm.call @a_setattr(obj, cstr(name), val)`; `mod.name=v` for A-module alias -> direct global store | `a_setattr` |
| destructure | :1191 | per-name `a_getitem(v, avalInt(i))` | `a_getitem` |
| index `a[i]` | :1613 | `llvm.call @a_getitem(obj, idx)` | `a_getitem` |
| field access `o.f` | :1652 | native const lookup, A-module member value, enum variant, else `a_getattr` | `a_getattr` |
| call `f(args)` A-fn | `callAFunction` :354, `evalFnArgs` :2767 | argv array = `[A_NONE env, arg0..]` via `argvPack`; `llvm.call @<cname>(%argv: !llvm.ptr) -> i64` | target fn |
| call builtin | `callBuiltin` :367 | `a_bi_call(builtinRuntimeId, argc, argv)` | `a_bi_call` |
| call dynamic value | `callValue` :374 | `a_call(fn, n, argv)` | `a_call` |
| method call `o.m(...)` | `evalMethodCall` :2479 | `a_method(obj, cstr(name), npos, nkw, argv)`; variadic packed to list; missing params padded `A_MISSING` | `a_method` |
| struct decl + ctor | `collectTopLevel` :605, `emitStructCtors` :980-1014 | ctor fn: `a_str(name)` + methods list + fns dict (`a_fnon_new(ptrtoint(@__a_mN), A_NONE, name)` + `a_dict_set`) -> `a_struct_new(name,methL,fnsD)`; then `a_dict_set(obj, field, argv[i+1])` per field; `a_promote`; return | `a_struct_new`, `a_dict_set`, `a_fnon_new`, `a_str`, `a_list_new`, `a_promote` |
| impl methods | `resolveImpls` :958, `pendingImpls_` :632 | appended to struct by name suffix match; emitted as `__a_m<seq>` fns | same as fn |
| enum / variant | `enumVariants_` :583, `variantValue`, `evalMatch` :1876 | dict-based: `{"__tag":..,"__name":..}` via `a_dict_set`/`a_dict_get`/`a_eq_slow`; match arms compare via `a_eq_slow`+icmp or `a_ge_slow`+`a_le_slow`+`llvm.and` | `a_dict_*`, `a_eq_slow`, `a_ge_slow`, `a_le_slow`, `a_match_fail` |
| closure / lambda | `evalLambda` :2030, `emitLambdas` :2335 | snapshot visible locals: `a_closure_env_new(n)` + `a_env_set(env,i,slot)` per capture; `a_fnon_new(ptrtoint(@__a_lam_N), env, "<lambda>")`; fn emitted later (LIFO drain) | `a_closure_env_new`, `a_env_set`, `a_fnon_new`, `a_env_get` |
| module / import | `collectTopLevel` :521-691 | A-module: `aModuleAliases_` map -> `resolveModuleFn`/`resolveModuleStruct` -> `callAFunction`/`__a_ctor_*`; `python:` -> `pythonAliases_` -> `py_import_module` builtin (id 164) or `a_bi_call` id 162 (`py_call`) with argv `[modstr, memberstr, nkw, pos..., (k,v)*]` via `evalPyCall` :2454; `native:` -> `nativeAliases_` -> `builtinRuntimeId` -> `callBuiltin`; `from x import y` -> `aModuleFnAliases_`/`moduleGlobalAliases_`/`moduleReexports_`/`pyMemberAliases_`/`builtinAliases_`/`constAliases_` | `a_bi_call` (162/164), module fns/globals |
| comptime | skipped at collect (:542 `fn->comptimeParams.empty()`); comptime exprs already folded by `pipeline/comptime.cpp` before emit | none emitted | none |
| try / error model | `evalTry` :1736 | `a_try_result(expr)` -> `a_is_propagate(v)` i32 -> `icmp ne` vs `i32 0` -> `llvm.cond_br` early-return path (defers+`a_promote`); Ok/Err/Some builtins ids 36/37/38 | `a_try_result`, `a_is_propagate`, `a_promote` |
| fibers / parallel | parfor path + `a_fib_*` builtins (ids 275-278) | `a_fib_spawn_handle(fn_ptr_as_i64, argc, argv)`, `a_fib_join_handle(h)`, plus builtin ids via `a_bi_call` | `a_fib_spawn_handle`, `a_fib_join_handle`, `a_fib_main` |
| GC roots | `emitGcRoots` :807-819 | `llvm.func @__a_gc_root_mark()`: per global `llvm.mlir.addressof` + `llvm.load` + `a_gc_mark_aval(v)`; emitted only if `rootSyms_` nonempty | `a_gc_mark_aval`, `a_gc_add_root_cb` |
| defer | stmt :1169, `endFunction` :904 | pushed to `pendingDefers_`, emitted LIFO before every return and at fallthrough | deferred expr's own calls |
| iterators / protocols | for-in path, `evalComprehension` :2369 | `a_iter_new`, `a_iter_next`; comprehensions loop + `a_list_append`/`a_set_add` + optional `a_truthy_raw_slow` filter | `a_iter_*`, `a_list_append`, `a_set_add`, `a_truthy_raw_slow` |
| function prologue | `beginFunction` :822, `emitFramePush` :887 | `llvm.func @<cname>(%argv: !llvm.ptr) -> i64`; params `llvm.getelementptr %argv[i+1]` + `llvm.load`; defaults via `icmp eq cur, kAvalTag` + `llvm.cond_br` to default-eval block; `a_frame_push(cstr(name), cstr(file), i32 line)` | `a_frame_push`, `a_frame_pop` |
| function epilogue | `emitReturn` :896, `endFunction` :904 | defers + `emitAllDrops` + `llvm.return <aval>`; fallthrough returns A_NONE | `a_frame_pop` |
| drops | `dropFnForExpr` :1219, `emitDropAt` :383, `popScope`, `emitAllDrops` | resolve struct `drop` method by inferred type; load slot (or `addressof` global `g:<sym>`); `callAFunction` on method | method fn |
| match stmt/expr | `evalMatch` :1876, `patternCond` :1758 | cond_br chain storing into alloca slot; patterns: literal->`a_eq_slow`, range->`a_ge_slow`+`a_le_slow`+and, variant->dict tag checks, tuple->`a_getitem` per elem; no-match -> A_NONE / `a_match_fail` | listed |
| safe-nav `?.` | `evalSafeNav` :1970 | None checks + `a_unwrap`/`a_getattr`/`a_is_option`/`a_some_new` | those |
| main wrapper | `emitMain` :1055 / `emitTestMain` :1082 | `llvm.func @main(%argc:i32,%argv0:!llvm.ptr)->i32`: `a_runtime_init()`, `a_gc_add_root_cb(@__a_gc_root_mark)`, `a_fib_main(ptrtoint(@__a_main_body), A_NONE, 0, null)` -> `llvm.return i32 0`; test mode builds names/fns stack arrays -> `a_test_main(n,names,fns)->i32` | `a_runtime_init`, `a_fib_main`, `a_test_main` |
| global init / top stmts | `emitMainBody` :1016 | single `__a_main_body` fn: globalInits (stores to `__a_g_*`) + topStmts in `graph_.order` module order + entry body (skipped in testMode) | per-stmt calls |

## 2. Runtime call catalog

Decl block `emitDecls` sigs[] (:697-763) — ~66 decls. Cross-ref `a_runtime.h`:

| Declared sig (MLIR) | a_runtime.h symbol | Used by |
|---|---|---|
| `(i64,i64)->i64` `a_add_slow`,`a_sub_slow`,`a_mul_slow`,`a_div_slow`,`a_floordiv_slow`,`a_mod_slow`,`a_pow` | `a_add_slow(aval,aval)` etc. | all BinOp arith |
| `(i64,i64)->i64` `a_eq_slow`,`a_ne_slow`,`a_lt_slow`,`a_le_slow`,`a_gt_slow`,`a_ge_slow` | compare `_slow` | BinOp compare, match literal, `??`-adjacent checks |
| `(i64,i64)->i64` `a_band_slow`,`a_bor_slow`,`a_bxor_slow`,`a_shl_slow`,`a_shr_slow` | bitwise `_slow` | BinOp bitwise |
| `(i64)->i64` `a_neg_slow`,`a_bnot_slow`,`a_not`,`a_truthy_raw_slow` | unary | Unary expr, condition truthiness, `not` |
| `(i64)->i64` `a_unwrap`,`a_some_new`,`a_try_result`,`a_promote` | option/result/try | `!`,`??`,try-expr,closures escapes |
| `(i64)->i64` `a_str`,`a_repr`,`a_len`,`a_int`,`a_float`,`a_bool`,`a_type_name`,`a_int_full`,`a_i64n` | conversions | literals, builtins paths, f-strings |
| `()->i64` `a_dict_new`,`a_set_new`,`a_builtin` | constructors | literals, `builtin` fn values |
| `(i64)->i64` `a_panic_msg`,`a_join_len`,`a_match_fail`,`a_range_bound`? (see note) | misc | errors, match, join |
| `(i64,i64,i64)->i64` `a_range_new` | range ctor | range expr, `in` lowering helpers |
| `(i64,i64)->i64` `a_getitem`,`a_setitem`,`a_contains`,`a_iter_new`,`a_closure_env_new`,`a_env_new`,`a_is_option?` no—i32 | seq protocol | index/assign/`in`/for |
| `(i64,i64,i64)->i64` `a_setitem`,`a_dict_set`,`a_env_set` | 3-arg setters | assign-index, dict lit, env capture |
| `(i64,i64,i64,i64)->i64` `a_slice` | slice | slice expr |
| `(i64,i64)->i64` `a_dict_get`,`a_dict_has` | dict read | match variant, `in` on dict |
| `(i64,i64)->i32` `a_is_option`,`a_is_propagate` | predicates | safe-nav, try |
| `(i64,!llvm.ptr)->i64` `a_str_new`,`a_char_new`,`a_str_joinsep` | str/char ctor, joinsep | string/char literals, join |
| `(i64,i64)->i64` `a_str_concat`,`a_list_append`,`a_list_append_int`,`a_set_add`,`a_fib_join_handle` | append/join | concat, list/set lit, parfor join |
| `(i64,!llvm.ptr)->i64` `a_list_new`,`a_tuple_new`,`a_str_join`,`a_fnon_new`,`a_fib_spawn_handle` | array/fn spawn | literals, closures, parfor |
| `(i64,i64,i64)->i64` `a_struct_new` | struct ctor | struct literal path |
| `(i64,!llvm.ptr,i64,i64,!llvm.ptr)->i64` `a_method` | dynamic method | `o.m(...)` |
| `(i64,!llvm.ptr)->i64` `a_getattr` | field get | `o.f` |
| `(i64,!llvm.ptr,i64)->i64` `a_setattr` | field set | `o.f = v` |
| `(i64,i64)->i64` `a_env_get` | env read | lambda/parfor body |
| `(i64,!llvm.ptr)->i64` `a_iter_next` | iterator step | for-in |
| `(i64,i64,!llvm.ptr)->i64` `a_call` | dynamic call | fn-value call |
| `(i64,i64,!llvm.ptr)->i64` `a_bi_call` | builtin dispatch | all builtins (id = `builtinRuntimeId`, `ic`->`ic_impl`) |
| `(!llvm.ptr,!llvm.ptr,i32)->()` `a_frame_push`; `()->()` `a_frame_pop` | stack frames | every fn prologue/epilogue |
| `(!llvm.ptr)->()` `a_pin_slot`,`a_gc_add_root_cb`; `(i64)->()` `a_gc_mark_aval`; `()->()` `a_gc_collect`,`a_runtime_init` | GC | pinning, roots, init |
| `()->i64` `a_now`; `(i64)->()` `a_sleep`; `(i64,!llvm.ptr)->()` `a_printv`,`a_eprintv`; `(i64)->()` `a_println` | IO/time | builtins (mostly via `a_bi_call`, decls kept for direct paths) |
| `(i64,i64,i64,!llvm.ptr)->i64` `a_fib_main`; `(i64,!llvm.ptr,!llvm.ptr)->i32` `a_test_main` | entry | `main` wrappers |

Notes: `a_runtime.h` provides INLINE fast paths for `a_add`/`a_eq`/etc. plus
exported `a_*_fast` variants (:568-589); the emitter calls ONLY `_slow`
variants — boxed-aval fast path is skipped entirely. `a_fnon_new(uint64_t entry, aval env, const char* name)` takes a raw fn pointer as u64 (hence `llvm.ptrtoint`). `a_method(aval obj, const char* name, uint64_t n, uint64_t nkw, aval* argv)`.

## 3. Lowering spec for the `a` dialect

Existing ops (AOps.td): `a.const` (int/float/bool/str/none attrs), `a.func`,
`a.call`, `a.return`, `a.yield`, `a.for` (index lo/hi/step + initArgs),
`a.while`+`a.condition`, `a.alloca`(i64 size, TypeAttr elem), `a.load`,
`a.store`, `a.gep`, `a.addressof`, `a.runtime.call`(StrAttr callee),
`a.fib.spawn`/`a.fib.join`/`a.fib.main`, `a.struct.getfield`/`setfield`,
`a.array.get`/`a.array.set`. Types (ATypes.td): `!a.aval`(->i64), `!a.ptr`,
`!a.struct<"N">`, `!a.array<N>` (unsigned only), `!a.enum<"N">`, `!a.none`, `!a.bool`.
ALowerToLLVM.cpp lowers via string-dispatch; successor-based cf.cond_br
convention (op0=cond i1, operands split by dest arg counts); region-based
`a.if` moves regions WITHOUT merge/phi support (phase-1 limitation).

| Construct | Recommended `a`-level op | Lowering |
|---|---|---|
| int/float/bool/None literal | `a.const` | `llvm.mlir.constant` encoded i64 (avalIntEncode/avalDbl rules) |
| string/char literal | `a.const str=` (extend with bytes+len) or new `a.strlit` | private `!llvm.array<N x i8>` global + insertvalue chain + `a_str_new`; or hoist to `a.runtime.call @a_str_new` |
| arith/compare/bitwise BinOp | new `a.binop` (kind attr: add/sub/.../in/not_in) OR keep `a.runtime.call @a_add_slow` | phase 1: `a.runtime.call` to `_slow` fn; phase 2 candidate: inline `llvm.add` etc. ONLY when both operands provably inline-tagged ints — requires new unboxing op, flag as non-goal |
| unary `- ~ ! not` | `a.unary` or `a.runtime.call` | `a.runtime.call @a_neg_slow` etc. |
| short-circuit and/or/?? | region-based `a.if` + `a.yield` (needs phi/merge support in ALowerToLLVM) or scf.if | scf.if with result, or keep alloca+cond_br lowering |
| if/while/for | `a.if`(region) / `a.while` / `a.for` | successor-based cf ops today; for-in needs iterator protocol -> keep runtime calls inside scf.for over index, or new `a.forin` lowering to iter_next loop |
| index/field assign | `a.array.set` / `a.struct.setfield` | already -> `a_setitem`/`a_setattr` |
| call A-fn | `a.call` (FlatSymbolRefAttr) | `llvm.call @cname(argv-ptr)` — needs ABI adapter: build argv alloca array `[A_NONE, args...]` |
| call builtin | `a.runtime.call @a_bi_call` with const id, or new `a.builtin<id>` | pack argv, call |
| method call | `a.runtime.call @a_method` | pack argv + cstr name global |
| struct/ctor | `a.struct.new` (new op) or sequence of `a.runtime.call`s | phase 1: emit the same call sequence (a_struct_new+a_dict_set+...) |
| enum variant | `a.runtime.call`s building dict + `a.const` tag/name | phase 1 dict-based |
| lambda/closure | `a.func` + `a.fnon.new` (new) + env capture ops | phase 1: `a.runtime.call @a_closure_env_new/@a_env_set/@a_fnon_new` with `llvm.ptrtoint` of lambda fn |
| module fn/global refs | `a.call`/`a.addressof` on `__a_f_m<id>_*`/`__a_g_<id>_*` | symbols already unique per module id |
| try `?` | new `a.try` or inline sequence | `a_try_result`+`a_is_propagate`+cond_br early return |
| fibers | `a.fib.spawn`/`a.fib.join` exist | phase-1 lower to `a_fib_spawn_handle`/`a_fib_join_handle` runtime calls |
| GC roots / main | NOT per-module ops — emitted once per program | keep as llvm.emitc-style tail or dedicated `a.program` wrapper op |

Constructs where boxed-aval semantics FORCE runtime calls regardless of
dialect richness: every BinOp/UnaryOp (operands are opaque i64 tagged
values), truthiness (`a_truthy_raw_slow`), `a_getitem`/`a_setitem`/
`a_getattr`/`a_setattr`/`a_method`/`a_call` (dynamic dispatch), all
collection constructors and mutators, `a_iter_*`, closures (`a_fnon_new`,
`a_env_*`), fibers, `a_bi_call`, `a_try_result`/`a_is_propagate`/`a_promote`,
GC (`a_pin_slot`, `a_gc_mark_aval`, `a_gc_add_root_cb`), frames
(`a_frame_push`/`a_frame_pop`). Only literals, local alloca/load/store,
branches, and direct A-fn calls are genuinely lowerable to pure
arith/index/cf/llvm without runtime involvement.

## 4. Bytecode-relevant notes (`.ambc` per-module serialization)

- Symbol names embed the module id but are NOT content-hashed:
  fns `__a_f_m<modid>_<name>_<seq>`, methods `__a_m<seq>`, ctors
  `__a_ctor_<i>`, lambdas `__a_lam_<n>`, parfors `__a_pf_<n>`, globals
  `__a_g_<modid>_<name>`. `<seq>` is a per-emission counter — stable only
  within one `emitMLIR` run over the whole `ModuleGraph`. Per-module
  serialization must either (a) serialize the whole merged module, or
  (b) re-derive identical seq numbering by emitting modules in
  `graph_.order` with a shared counter.
- String-data globals `__a_str<N>`/`__a_cstr<N>`/`__a_chr<N>` are numbered
  by a global counter in emission order; same caveat.
- `__a_main_body` aggregates ALL modules: globalInits + topStmts run in
  `graph_.order` (dependency order), entry body last. This is inherently
  whole-program — cannot be split per-module without changing init order
  semantics. `.ambc` must preserve either the merged body or an ordered
  list of per-module init thunks.
- `__a_gc_root_mark` covers globals from every module (`rootSyms_` = all
  `emitGlobals` syms). Program-global, emitted once.
- `main`/`__a_main_body`/`__a_gc_root_mark`/runtime decls are
  program-global, not per-module. The ~66 `llvm.func @a_*` declarations
  are pure decls (no body) and can be deduplicated/omitted per module if
  the linker provides them.
- Lambda/parfor queues (`pendingLambdas_`, `pendingParfors_`) drain at end
  of `run()` — lambdas defined in module A's functions are emitted in the
  same module block; ordering is LIFO. Per-module split must keep lambda
  defs adjacent to (or at least symbol-resolvable from) their parent fn.
- `fnCnames_` is keyed by BARE fn name across the whole graph; Ident
  resolution order is locals -> globals -> same-named fn -> builtins.
  Cross-module name shadowing is resolved at collect time, so emitted
  symbols are already unambiguous — safe to serialize per module.
- testMode changes `main` -> `a_test_main` wrapper and skips entry body;
  `testFilter` affects which fns are emitted. Bytecode packaging must
  record which mode produced the module.

## 5. Gaps vs Mojo-style optimizable IR

Sample: `/tmp/nd.mlir` from `examples/stdlib/ndarray_indexing.astra`
(1.56 MB, 29,911 lines; heavy stdlib imports).

Op census:

| Op | Count |
|---|---|
| `llvm.call` | 1,575 |
| `llvm.mlir.constant` | 11,654 |
| `llvm.insertvalue` | 9,327 |
| `llvm.mlir.global` | 525 |
| `llvm.func @` | 185 (incl ~66 runtime decls) |
| `llvm.alloca` | 581 |
| `llvm.cond_br` | 157 |
| `llvm.br` | 243 |
| `llvm.return` | 662 |
| `llvm.icmp` | 157 |
| `llvm.ptrtoint` | 34 |
| `llvm.add/sub/mul` (inline arith) | **0** |

llvm.call breakdown (top): `a_getattr` 140, `a_frame_pop` 137,
`a_truthy_raw_slow` 119, `a_frame_push` 91, `a_list_new` 88, `a_str_new` 87,
`a_add_slow` 86, `a_getitem` 82, `a_method` 81, `a_bi_call` 79,
`a_list_append` 74, `a_eq_slow` 54, `a_dict_set` 42, `a_pin_slot` 38,
`a_iter_next`/`a_iter_new` 38 each, `a_fnon_new` 33, `a_mul_slow` 33,
`a_lt_slow` 30, plus user-fn calls (`__a_f_*`, `__a_ctor_*`, `__a_m*`).

Unoptimizable-by-construction properties:

- Every value is a boxed i64 aval; there is not a single inline
  `llvm.add/sub/mul` on program values. All arithmetic, comparison,
  truthiness, indexing, attribute access, iteration, and calls go through
  opaque runtime calls — LLVM cannot see through them (no `memory(none)`
  attributes are emitted either, though the callees mostly don't write
  through the aval args).
- mem2reg CAN promote the 581 allocas (they are plain i64 slots, loads/
  stores only) BUT most slots escape: `a_pin_slot` is called on iterator
  item slots (:38 sites), `argvPack`/`callAFunction` pass `!llvm.ptr`
  arrays to runtime calls, closure env capture stores slot addresses via
  `a_env_set` semantics — escape analysis will bail on a large fraction.
- 9,327 `llvm.insertvalue` ops: private string-data globals are built
  byte-by-byte (`undef` + N x (constant + insertvalue)). For a 75-byte
  cstr that is ~150 ops per global. These are inside `private constant`
  globals so LLVM folds them at compile time, but they dominate IR size
  and slow parsing/opt. A `llvm.mlir.constant` of an array or
  `llvm.global_constant` with a blob would collapse this.
- No `scf`/`arith`/`cf`/`vector`/`index` dialect usage at all — control
  flow is raw successor-based `llvm.cond_br`/`llvm.br` with numeric
  `^L<n>` labels; loops are manual branches, not `scf.for`. Nothing for
  loop idiom recognition, LICM, or vectorization to latch onto.
- No mem2reg targets beyond allocas; no `llvm.memcpy`/aggregate values —
  structs are heap dicts, tuples are heap arrays. SROA is useless.
- Indirect-call heavy: `a_call`, `a_method`, `a_bi_call`, `a_fib_main`
  all dispatch through i64 function pointers (`ptrtoint` sites = 34);
  devirtualization impossible without runtime type info.
- Constant avals are already folded to i64 constants at emit time
  (`llvm.mlir.constant` count 11,654) — that part IS optimizable, but
  every use site still routes through `_slow` calls which re-dispatch on
  tags at runtime.

Net: the emitted IR is a faithful but maximally-boxed lowering. Phase-1
`a`-dialect work inherits this; meaningful optimization requires either
(un)boxing-aware ops (`a.int.add` with `!a.int` type + runtime fallback)
or whole-program points-to analysis the current pipeline does not have.
