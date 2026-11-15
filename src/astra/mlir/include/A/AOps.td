//===- AOps.td - A dialect operation definitions -------------*- tablegen -*-===//
//
// Phase-1 op set for the `a` dialect. The guiding rule (docs/mlir-architecture.md)
// is to reuse upstream dialects for generic semantics: arithmetic goes to
// `arith`/`math`, memory to `llvm` after lowering, and structured control flow
// mirrors `scf` (`a.for`/`a.while`/`a.yield`/`a.condition`). The `a` dialect
// only carries A-specific semantics:
//
//   * aval boxing (`!a.aval` = tagged i64 payload) and literals (`a.const`)
//   * nominal aggregates (`a.struct.*`, `a.array.*`) lowered to runtime calls
//   * calls into the `a_*` C runtime (`a.runtime.call`, `a.call`)
//   * fibers (`a.fib.*`)
//   * function/module-level constructs (`a.func`, `a.return`)
//
// Every op documents its eventual lowering to the LLVM dialect in comments.
//
//===----------------------------------------------------------------------===//

#ifndef A_OPS_TD
#define A_OPS_TD

include "A/ADialect.td"
include "A/ATypes.td"

include "mlir/IR/BuiltinAttributes.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

// Base class for all `a` dialect ops.
class A_Op<string mnemonic, list<Trait> traits = []>
    : Op<A_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// a.const: literal materialization
//===----------------------------------------------------------------------===//

// `A_ConstKindAttr` is an I64EnumAttr; `mlir-tblgen -gen-enum-decls/-defs`
// generates both the C++ `enum class AConstKind` and the `AConstKindAttr`
// attribute wrapper (same pattern as `arith::CmpIPredicateAttr`). The
// TableGen name doubles as the op-argument attribute type.
def A_ConstKindAttr : I64EnumAttr<
    "AConstKind", "",
    [
      // C++ symbol names avoid language keywords (int/float/bool); the
      // third field keeps the printed spelling used in MLIR text.
      I64EnumAttrCase<"Int", 0, "int">,
      I64EnumAttrCase<"Float", 1, "float">,
      I64EnumAttrCase<"Bool", 2, "bool">,
      I64EnumAttrCase<"Str", 3, "str">,
      I64EnumAttrCase<"None", 4, "none">,
    ]> {
  let cppNamespace = "::mlir::a";
}

// `a.const` produces a boxed `!a.aval` from a literal.
//
//   %1 = a.const "int"(%0 : i64) -> !a.aval
//   %2 = a.const "float"(%0 : f64) -> !a.aval
//   %3 = a.const "bool"(%0 : i64) -> !a.aval
//   %4 = a.const "str" {str = "hello"} -> !a.aval
//   %5 = a.const "none" -> !a.aval
//
// Lowering to LLVM (phase 1):
//   int/bool : if |v| < kInlineLim -> llvm.mlir.constant of the tagged i64
//              encoding; else llvm.call @a_i64n(i64).
//   float    : llvm.mlir.constant of the bit-cast i64 payload.
//   str      : emit `llvm.mlir.global private constant @__a_strN` of
//              `!llvm.array<len x i8>` bytes, llvm.mlir.addressof, then
//              llvm.call @a_str_new(!llvm.ptr, i64 len).
//   none     : llvm.mlir.constant of the tagged-none aval (kAvalTag | 0x2).
// NOT ConstantLike/Pure: `a.const str` calls runtime a_str_new and even
// int/float consts materialize avals — the canonicalizer would hoist
// ConstantLike ops out of function bodies to module level, breaking
// dominance for the lowering.
def AConstOp : A_Op<"const"> {
  let summary = "A literal constant";
  let description = [{
    Materializes an A literal as a boxed `!a.aval`. The `kind` enum selects
    the literal class; `int`/`bool` take an `i64` operand, `float` takes an
    `f64` operand, `str` carries the UTF-8 text in the `str` attribute, and
    `none` has no payload. Custom assembly: see AOps.cpp.
  }];

  let arguments = (ins
    A_ConstKindAttr:$kind,
    OptionalAttr<I64Attr>:$ivalue,
    OptionalAttr<F64Attr>:$fvalue,
    OptionalAttr<StrAttr>:$str);
  let results = (outs AVAL:$result);

  // `a.const int {ivalue = 5 : i64} -> !a.aval`
  // `a.const float {fvalue = 1.5 : f64} -> !a.aval`
  // `a.const bool {ivalue = 1 : i64} -> !a.aval`
  // `a.const str {str = "hello"} -> !a.aval`
  // `a.const none -> !a.aval`
  // `kind` prints/parses as its symbolic enum spelling (int/float/bool/str/
  // none); the payload attributes stay in the attribute dict.
  let assemblyFormat = "$kind attr-dict `->` type(results)";

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// a.func / a.call / a.return: function-level structure
//===----------------------------------------------------------------------===//

// `a.func` is the A-level function. Like `func.func` it has a symbol name, a
// `function.type` attribute and a single region; phase 1 bodies keep boxed
// aval values and lower to `llvm.func` (arguments unboxed from the `!a.ptr`
// argv array, matching mlir_emit.cpp).
def AFuncOp : A_Op<"func", [FunctionOpInterface, OpAsmOpInterface]> {
  let summary = "An A function";
  let description = [{
    Defines an A function. The region body uses `!a.aval`/`!a.ptr` values and
    `a.*` ops; lowering converts it to an `llvm.func` whose parameters are
    unboxed from the `!a.ptr` argv array (argv[i+1] = i-th argument).
  }];

  let arguments = (ins
    SymbolNameAttr:$sym_name,
    TypeAttrOf<FunctionType>:$function_type,
    OptionalAttr<StrAttr>:$sym_visibility,
    OptionalAttr<DictArrayAttr>:$arg_attrs,
    OptionalAttr<DictArrayAttr>:$res_attrs);
  let regions = (region AnyRegion:$body);

  let builders = [OpBuilder<(ins
    "StringRef":$name, "FunctionType":$type,
    CArg<"ArrayRef<NamedAttribute>", "{}">:$attrs,
    CArg<"ArrayRef<DictionaryAttr>", "{}">:$argAttrs)
  >];

  // NOTE: generic assembly format is used for phase 1 (attrs incl.
  // function_type/sym_name print in the attr-dict). A func.func-style custom
  // format can be added later without changing the op signature.
  //
  // Custom func.func-style assembly (implemented in AOps.cpp):
  //   a.func @name(%arg0: !a.aval, %arg1: i64) -> !a.aval, i64
  //       {sym_visibility = "public", ...} {
  //     ...
  //   }
  // Arg names are printed only when the body is present (they come from the
  // entry block arguments); external declarations print bare types. The
  // attribute dict carries sym_visibility/arg_attrs/res_attrs; sym_name and
  // function_type are consumed by the signature itself.
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Returns the region on the current operation that is callable, or null
    /// for external declarations.
    ::mlir::Region *getCallableRegion() {
      return isExternal() ? nullptr : &getBody();
    }

    ArrayRef<Type> getArgumentTypes() {
      return getFunctionType().getInputs();
    }

    ArrayRef<Type> getResultTypes() {
      return getFunctionType().getResults();
    }

    bool isDeclaration() { return isExternal(); }

    //===------------------------------------------------------------------===//
    // OpAsmOpInterface
    //===------------------------------------------------------------------===//

    /// Allow the dialect prefix to be omitted.
    static StringRef getDefaultDialect() { return "a"; }
  }];
}

// `a.call` is a direct call to an `a.func` within the same symbol scope.
// Operands/results are `AnyType` in the op definition so phase-2 typed
// scalars can flow through without changing the op; the verifier checks the
// operand/result types against the callee's `function.type`.
//
// Lowering: unwrap `!a.aval` args to raw i64 payloads, llvm.call the lowered
// `llvm.func`, re-box the result.
def ACallOp : A_Op<"call",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "direct call to an `a.func`";
  let description = [{
    Calls a function defined by `a.func` in the same symbol scope. The callee
    is a `FlatSymbolRefAttr` named `callee`; operand and result types must
    match the callee's `function.type`.
  }];

  let arguments = (ins FlatSymbolRefAttr:$callee,
                       Variadic<AnyType>:$operands);
  let results = (outs Variadic<AnyType>);

  let assemblyFormat = [{
    $callee `(` $operands `)` attr-dict `:` functional-type($operands, results)
  }];

  let hasVerifier = 1;
}

// `a.return` returns from an `a.func`. Phase 1 functions return a single
// boxed `!a.aval` (the tagged-none aval for no meaningful value).
//
// Lowering: llvm.return of the unboxed i64 payload.
def AReturnOp : A_Op<"return",
    [Terminator, ReturnLike,
     HasParent<"AFuncOp">]> {
  let summary = "return from an `a.func`";
  let description = [{
    Returns a value from the enclosing `a.func`. With no operand it returns
    the tagged-none aval, matching `llvm.return` with no value.
  }];

  let arguments = (ins Optional<AVAL>:$value);
  let assemblyFormat = "attr-dict ($value^ `:` type($value))?";

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// a.to_aval / a.from_aval: boxing boundary for typed scalars (phase 2 / M1)
//===----------------------------------------------------------------------===//

// `a.to_aval` boxes a typed scalar into the `!a.aval` tagged-i64 payload.
// Operand and result are the *only* types allowed: any M1 scalar
// (`iN` integer, `fN` float, `i1` bool) in, `!a.aval` out.
//
//   %b = a.to_aval %x : i64 -> !a.aval
//   %f = a.to_aval %d : f64 -> !a.aval
//   %t = a.to_aval %c : i1 -> !a.aval
//
// Boxing semantics mirror the phase-1 emitter/runtime
// (src/astra/frontend/mlir/mlir_emit.cpp `avalInt`/`avalDbl`/`avalBool`,
// src/astra/runtime/a_runtime.h `a_mkint`/`a_mkdouble`):
//   iN  : extended to i64 (zext when `uvalue` is set, sext otherwise; for
//         N < 64 the emitter's `uN` spellings arrive as zext too), then if
//         -2^46 <= v < 2^46 encoded inline as
//         `A_TAG | (((v << 1) | 1) & 0x0000FFFFFFFFFFFF)`; otherwise lowered
//         to `llvm.call @a_i64n(i64) -> i64` (the full-range heap-boxed
//         integer path). NOTE: for `i64` operands the emitter's `uvalue`
//         attribute is the only signedness signal; when absent the value is
//         treated as signed (the common `int` case).
//   fN  : extended to f64 (fpext) if narrower, NaN canonicalized (any NaN
//         becomes the quiet NaN 0x7FF8000000000000, matching runtime
//         `mkdouble`, so NaN payloads cannot collide with the tag space),
//         then bitcast to i64 -- the aval double payload is the raw f64 bit
//         pattern.
//   i1  : `icmp eq` against 1 then select kAvalTrue (A_TAG|6) / kAvalFalse
//         (A_TAG|4).
//
// `a.to_aval` on an `!a.aval` operand is the identity (verifier-admitted
// no-op; `a-lower-typed` forwards the operand).
//
// Lowering: emitted inline by the `a-lower-typed` pass (docs/
// mlir-typed-values-design.md M1); never reaches `a-lower-to-llvm`.
def AToAvalOp : A_Op<"to_aval", [Pure]> {
  let summary = "box a typed scalar into !a.aval";
  let description = [{
    Converts a phase-2 unboxed scalar (`iN`, `fN`, or `i1`) into the boxed
    `!a.aval` representation (tagged i64). See the op comment block above for
    the exact encoding. `uvalue`, when present, marks an unsigned integer
    operand (the only signedness signal for `i64`; narrower widths use
    zext/sext per this flag as well).
  }];

  let arguments = (ins
    AnyType:$value,
    OptionalAttr<I64Attr>:$uvalue);
  let results = (outs AVAL:$result);

  let assemblyFormat = [{
    $value attr-dict `:` type($value) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.from_aval` unboxes an `!a.aval` into a typed scalar. The result type is
// the *target* scalar type (`iN`, `fN`, or `i1`); the operand must be
// `!a.aval`.
//
//   %x = a.from_aval %a : !a.aval -> i64
//   %d = a.from_aval %a : !a.aval -> f64
//   %c = a.from_aval %a : !a.aval -> i1
//
// Decoding (per docs/mlir-typed-values-design.md §3):
//   iN  : inline ints (tag bits set and payload bit0 == 1) decode as
//         `(payload >> 1)` with sign extension from bit 46, then
//         trunc/sext to iN. Any other aval word (notably heap-boxed T_INT64
//         values produced by `a_i64n`) goes through `llvm.call
//         @a_int_full(i64) -> i64`, which unboxes inline and boxed ints and
//         panics on non-ints -- the *unchecked ABI unpack*: it is only
//         legal where Sema proves the value is that scalar (function call
//         boundaries, typed loads). Mid-expression checked narrowing is a
//         separate future op (`a.unbox<T>`), not this one.
//   fN  : requires an untagged aval (top 16 bits clear); the i64 word is
//         bitcast to f64 then fptrunc/fpext to fN. Tag mismatch traps.
//   i1  : compares the aval word against kAvalTrue/kAvalFalse
//         (A_TAG|6 / A_TAG|4); tag mismatch traps.
//
// The `uvalue` attribute marks unsigned decode for `iN` results (zext instead
// of sext); for `i64` it is required to distinguish u64 from int.
//
// Lowering: emitted inline by the `a-lower-typed` pass; never reaches
// `a-lower-to-llvm`.
def AFromAvalOp : A_Op<"from_aval", [Pure]> {
  let summary = "unbox !a.aval into a typed scalar";
  let description = [{
    Unchecked ABI unpack of a boxed `!a.aval` into the scalar result type.
    Only emitted where Sema has proven the dynamic type (see the op comment
    block); int operands that are not inline ints are unboxed via
    `@a_int_full`, other tag mismatches are `llvm.trap` at runtime, not a
    recoverable error. `uvalue` marks unsigned integer decode.
  }];

  let arguments = (ins
    AVAL:$value,
    OptionalAttr<I64Attr>:$uvalue);
  let results = (outs AnyType:$result);

  let assemblyFormat = [{
    $value attr-dict `:` type($value) `->` type(results)
  }];

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Structured control flow (mirrors scf.for/scf.while semantics)
//===----------------------------------------------------------------------===//

// `a.yield` is the region terminator for `a.for`/`a.while` bodies; semantics
// are identical to `scf.yield` (operand types must match the parent op's
// carried values/results).
//
// Lowering: erased during `a` -> `cf`/`llvm` lowering; operands become the
// corresponding block arguments / phi inputs.
def AYieldOp : A_Op<"yield",
    [Pure, Terminator,
     DeclareOpInterfaceMethods<RegionBranchTerminatorOpInterface>,
     ParentOneOf<["AForOp", "AWhileOp"]>]> {
  let summary = "yield from an `a.for`/`a.while` region";
  let description = [{
    Terminates a region of `a.for` or `a.while`, yielding values to the
    parent operation. Operand types must match the parent op's init_args
    (for `a.for`) or inits/results (for `a.while`).
  }];

  let arguments = (ins Variadic<AnyType>:$operands);
  let assemblyFormat = "attr-dict ($operands^ `:` type($operands))?";

  let hasVerifier = 1;
}

// `a.for` is a counted loop: `for iv in [lo, hi) step st` with optional
// loop-carried `iter_args`. Bounds/step are `index`; iter_args/results are
// AnyType (boxed aval in phase 1). Semantics mirror `scf.for`.
//
//   %r = a.for %i = %lo to %hi step %st iter_args(%a = %init) -> !a.aval {
//     %next = ...
//     a.yield %next : !a.aval
//   }
//
// Lowering: cf loop nest; `icmp ult` on the index IV; iter_args become phi
// nodes; `a.yield` operands feed the latch branch.
def AForOp : A_Op<"for",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface,
        ["getEntrySuccessorOperands"]>,
     SingleBlock]> {
  let summary = "counted for loop";
  let description = [{
    Counts the induction variable from `%lowerBound` to `%upperBound`
    (exclusive) by `%step`. `%initArgs` are loop-carried values; the body
    region takes the induction var plus the init args as block arguments and
    must terminate in `a.yield` producing the next iter_args. Results are the
    final iter_args values.
  }];

  let arguments = (ins
    Index:$lowerBound,
    Index:$upperBound,
    Index:$step,
    Variadic<AnyType>:$initArgs);
  let results = (outs Variadic<AnyType>:$results);
  let regions = (region SizedRegion<1>:$body);

  // Custom assembly (implemented in AOps.cpp) because the iter_args and
  // result type lists are optional and would otherwise leave stray separators:
  //
  //   %r = a.for %lo to %hi step %st iter_args(%a, %b) : index to index
  //     step index, !a.aval, !a.aval -> !a.aval, !a.aval { ^bb0(%iv: index,
  //     %a2: !a.aval, %b2: !a.aval): ... a.yield %next : !a.aval, !a.aval }
  //
  // Unlike scf.for the iter_args are printed as bare values (no
  // `name = value` bindings); the loop-carried names live on the body
  // region's block arguments. The `->` clause shares the init_args group
  // because result types always match the iter_args types.
  let hasCustomAssemblyFormat = 1;

  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// The induction variable is the first block argument of the body.
    BlockArgument getInductionVar();
    /// Lower/upper/step bound operands (index-typed).
    Value getLowerBoundOperand();
    Value getUpperBoundOperand();
    Value getStepOperand();
    /// Loop-carried iter args: block arguments 1..n of the body.
    ValueRange getRegionIterArgs();
    /// Operands of the body's `a.yield` terminator.
    ValueRange getYieldedValues();
    void setYieldedValues(ValueRange newValues);
    // NOTE: getInitArgs()/getResults()/getBody() etc. are generated by ODS.
  }];
}

// `a.while` is a generic while/do-while: the `before` region computes the
// condition via `a.condition`, the `after` region is the loop body ending in
// `a.yield`. Mirrors `scf.while` exactly.
//
//   %r = a.while (%a = %init) : (!a.aval) -> !a.aval {
//     %cond = ...
//     a.condition(%cond) %a : !a.aval
//   } do {
//   ^bb0(%a2: !a.aval):
//     a.yield %next : !a.aval
//   }
//
// Lowering: cf regions + blocks; `a.condition` lowers to cf.cond_br into the
// `after` block or to the merge block.
def AWhileOp : A_Op<"while",
    [DeclareOpInterfaceMethods<RegionBranchOpInterface,
        ["getEntrySuccessorOperands"]>,
     SingleBlock]> {
  let summary = "generic while loop";
  let description = [{
    A two-region loop. The `before` region must end in `a.condition`, whose
    first operand is an `i1`; the remaining operands are forwarded either to
    the `after` region (condition true) or to the `a.while` results (false).
    The `after` region ends in `a.yield` whose operands feed back as the next
    `before` region arguments.
  }];

  let arguments = (ins Variadic<AnyType>:$inits);
  let results = (outs Variadic<AnyType>:$results);
  let regions = (region SizedRegion<1>:$before, SizedRegion<1>:$after);

  // Custom assembly (implemented in AOps.cpp), mirroring scf.while:
  //
  //   %r = a.while (%a, %b) : (!a.aval, !a.aval) -> !a.aval, !a.aval
  //     { ^bb0(%a2: !a.aval, %b2: !a.aval): ... a.condition(%c) (%x : !a.aval) }
  //     do { ^bb0(%x2: !a.aval): ... a.yield %next : !a.aval }
  //
  // Init values print without `name =` bindings (names live on block args).
  let hasCustomAssemblyFormat = 1;

  let hasVerifier = 1;

  let extraClassDeclaration = [{
    // getBefore()/getAfter() are generated by ODS (region accessors).
    Block *getBeforeBlock() { return &getBefore().front(); }
    Block *getAfterBlock() { return &getAfter().front(); }
  }];
}

// `a.condition` is the terminator of an `a.while` `before` region. The first
// operand is the `i1` continue-condition; the trailing operands are forwarded
// to the `after` region on true, or become `a.while` results on false.
//
// Lowering: cf.cond_br on the condition operand.
def AConditionOp : A_Op<"condition",
    [Terminator,
     DeclareOpInterfaceMethods<RegionBranchTerminatorOpInterface>]> {
  let summary = "while-loop continuation condition";
  let description = [{
    Terminator of the `before` region of `a.while`. Operand 0 is the
    continuation condition (`i1`); operands 1..n are forwarded to the `after`
    region when the condition is true, or yielded as `a.while` results when
    false.
  }];

  let arguments = (ins I1:$condition, Variadic<AnyType>:$args);
  let assemblyFormat = [{
    `(` $condition `)` attr-dict ($args^ `:` type($args))?
  }];

  let hasVerifier = 1;

}

//===----------------------------------------------------------------------===//
// Memory: alloca / load / store / gep / addressof
//===----------------------------------------------------------------------===//
// These mirror the `llvm.alloca`/`llvm.load`/`llvm.store`/
// `llvm.getelementptr`/`llvm.mlir.addressof` ops emitted by mlir_emit.cpp,
// but at the `a` level so the frontend does not depend on the LLVM dialect.

// `a.alloca` reserves `size` elements of `element_type` on the stack and
// returns an `!a.ptr`. `element_type` is a TypeAttr (an `!a.*` type or a
// builtin type) used by the lowering to pick the LLVM element type.
//
// Lowering: llvm.alloca of `size x <lowered element type>`; phase 1 element
// types are i64 (aval slot) or i8-array (argv pack).
def AAllocaOp : A_Op<"alloca",
    [MemoryEffects<[MemAlloc<AutomaticAllocationScopeResource>]>]> {
  let summary = "stack allocation";
  let description = [{
    Allocates `size` elements of `element_type` on the stack and returns an
    `!a.ptr` to the first element.
  }];

  let arguments = (ins I64:$size, TypeAttr:$element_type);
  let results = (outs APtr:$result);

  let assemblyFormat = [{
    $size `x` $element_type attr-dict `:` type($size) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.load` loads a value through an `!a.ptr`. Result type is `AnyType` so
// phase 2 can load unboxed scalars; phase 1 loads produce `!a.aval`.
//
// Lowering: llvm.load of the pointer's pointee type (phase 1: i64).
def ALoadOp : A_Op<"load",
    [MemoryEffects<[MemRead<DefaultResource>]>]> {
  let summary = "load through a pointer";
  let description = [{
    Loads a value of the result type from the address in `ptr`.
  }];

  let arguments = (ins APtr:$ptr);
  let results = (outs AnyType:$result);

  let assemblyFormat = [{
    $ptr attr-dict `:` type($ptr) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.store` stores `value` through an `!a.ptr`.
//
// Lowering: llvm.store.
def AStoreOp : A_Op<"store",
    [MemoryEffects<[MemWrite<DefaultResource>]>]> {
  let summary = "store through a pointer";
  let description = [{
    Stores `value` at the address in `ptr`.
  }];

  let arguments = (ins AnyType:$value, APtr:$ptr);
  let assemblyFormat = [{
    $value `,` $ptr attr-dict `:` type($value) `,` type($ptr)
  }];

  let hasVerifier = 1;
}

// `a.gep` computes `ptr + index` elements. Phase 1 supports a single i64
// index (matches the emitter's `llvm.getelementptr ptr[idx]`); phase 2 may
// accept structured indices.
//
// Lowering: llvm.getelementptr; phase 1 treats the pointee as i64 elements
// (the lowering pass refines via `a.alloca`'s element_type).
def AGetElementPtrOp : A_Op<"gep", [Pure]> {
  let summary = "pointer arithmetic";
  let description = [{
    Returns `ptr` offset by `index` elements.
  }];

  let arguments = (ins APtr:$ptr, I64:$index);
  let results = (outs APtr:$result);

  let assemblyFormat = [{
    $ptr `[` $index `]` attr-dict `:`
    `(` type($ptr) `,` type($index) `)` `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.addressof` materializes the address of a module-level symbol (global
// slot or private string-data global) as an `!a.ptr`.
//
// Lowering: llvm.mlir.addressof @sym.
def AAddressofOp : A_Op<"addressof", [Pure]> {
  let summary = "address of a module-level symbol";
  let description = [{
    Returns an `!a.ptr` to the module-level symbol named by `sym_name`.
  }];

  let arguments = (ins FlatSymbolRefAttr:$sym_name);
  let results = (outs APtr:$result);

  let assemblyFormat = [{
    $sym_name attr-dict `:` type(results)
  }];

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Calls: a.call (A funcs), a.runtime.call (a_* C runtime), a.fib.*
//===----------------------------------------------------------------------===//

// `a.runtime.call` is the escape hatch to the `a_*` C runtime functions
// (a_add_slow, a_str_new, a_getattr, a_fib_spawn_handle, ...). `callee` is the
// C symbol name; `signature`, when present, carries the LLVM-level signature
// (e.g. "(i64, i64) -> i64") used by the verifier/lowering; without it
// operands/results default to phase-1 `!a.aval` boxing.
//
// Lowering: llvm.call @<callee>(unboxed args) : <signature or inferred>.
def ARuntimeCallOp : A_Op<"runtime.call"> {
  let summary = "call an `a_*` C runtime function";
  let description = [{
    Direct call to a function in the A C runtime (compiler/astra/runtime).
    All runtime functions follow the phase-1 ABI: i64/!llvm.ptr in, i64 or
    void out. `signature`, when present, is the LLVM function type used for
    verification and lowering.
  }];

  let arguments = (ins
    StrAttr:$callee,
    OptionalAttr<StrAttr>:$signature,
    Variadic<AnyType>:$operands);
  let results = (outs Variadic<AnyType>);

  // `signature` stays in the attribute dict: a bare `:`-prefixed optional
  // clause here would greedily consume the functional-type colon that
  // follows, making the op unparsable. Printed form:
  //   a.runtime.call "a_add_slow"(%a, %b) {signature = "(i64, i64) -> i64"}
  //     : (!a.aval, !a.aval) -> !a.aval
  let assemblyFormat = [{
    $callee `(` $operands `)` attr-dict
    `:` functional-type($operands, results)
  }];

  let hasVerifier = 1;
}

// `a.fib.spawn` spawns `fib_fn` as a fiber with `args` (packed aval array)
// and returns an `i64` fiber handle. Wraps `a_fib_spawn_handle`.
//
// Lowering: llvm.call @a_fib_spawn_handle(i64 fnptr, i64 nargs, !llvm.ptr
// args) -> i64.
def AFibSpawnOp : A_Op<"fib.spawn"> {
  let summary = "spawn a fiber";
  let description = [{
    Spawns a fiber running `fib_fn` with `nargs` boxed arguments in `args`
    (an `!a.aval` array pointer). Returns an `i64` handle for `a.fib.join`.
  }];

  let arguments = (ins
    AVAL:$fib_fn,
    I64:$nargs,
    AVAL:$args);
  let results = (outs I64:$handle);

  let assemblyFormat = [{
    $fib_fn `,` $nargs `,` $args attr-dict `:`
    type($fib_fn) `,` type($nargs) `,` type($args) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.fib.join` waits for the fiber handle and returns its `!a.aval` result.
// Wraps `a_fib_join_handle`.
//
// Lowering: llvm.call @a_fib_join_handle(i64) -> i64, re-boxed as aval.
def AFibJoinOp : A_Op<"fib.join"> {
  let summary = "join a fiber";
  let description = [{
    Blocks until the fiber identified by `handle` completes and returns its
    result as a boxed `!a.aval`.
  }];

  let arguments = (ins I64:$handle);
  let results = (outs AVAL:$result);

  let assemblyFormat = [{
    $handle attr-dict `:` type($handle) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.fib.main` is the fiber entry trampoline: the runtime calls it with the
// fiber's boxed args; it dispatches to the A-level entry function.
// Wraps `a_fib_main`.
//
// Lowering: llvm.call @a_fib_main(i64, i64, !llvm.ptr, i64) -> i64.
def AFibMainOp : A_Op<"fib.main"> {
  let summary = "fiber entry trampoline";
  let description = [{
    Entry point invoked by the fiber scheduler (`a_fib_main` in the runtime).
    Unpacks the fiber args and produces the fiber's result aval.
  }];

  let arguments = (ins
    I64:$fnptr,
    I64:$argc,
    APtr:$argv,
    AVAL:$env);
  let results = (outs AVAL:$result);

  let assemblyFormat = [{
    $fnptr `,` $argc `,` $argv `,` $env attr-dict `:`
    type($fnptr) `,` type($argc) `,` type($argv) `,` type($env)
    `->` type(results)
  }];

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Aggregate access: struct fields and array elements
//===----------------------------------------------------------------------===//
// Phase 1 lowers these to the generic runtime calls a_getattr/a_setattr and
// a_getitem/a_setitem on boxed aval values; the nominal types (!a.struct,
// !a.array) let phase 2 lower them to direct memory ops.

// `a.struct.getfield` reads field `index` of the struct value `obj`.
// `struct_type`, when present, pins the nominal `!a.struct<"Name">`.
// `field`, when present, is the source-level field name (StrAttr); the
// positional `index` operand remains required for phase-2 GEP lowering.
//
// Lowering (phase 1): llvm.call @a_getattr(i64 obj, !llvm.ptr name) -> i64,
// where `name` is materialized as a private C-string global. Phase 2: direct
// GEP+load on the unboxed struct.
def AStructGetFieldOp : A_Op<"struct.getfield"> {
  let summary = "read a struct field";
  let description = [{
    Returns the value of field `index` (0-based declaration order) of boxed
    struct `obj`.
  }];

  let arguments = (ins
    AVAL:$obj,
    I64:$index,
    OptionalAttr<StrAttr>:$field,
    OptionalAttr<TypeAttrOf<AStruct>>:$struct_type);
  let results = (outs AVAL:$result);

  // `struct_type` stays in the attribute dict: a bare `:`-prefixed optional
  // clause would greedily consume the operand-type colon that follows.
  let assemblyFormat = [{
    $obj `[` $index `]` attr-dict
    `:` type($obj) `,` type($index) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.struct.setfield` writes `value` into field `index` of `obj` and returns
// the (possibly re-boxed) struct. Wraps `a_setattr`.
// `field`, when present, is the source-level field name (StrAttr); see
// `a.struct.getfield` for details.
//
// Lowering (phase 1): llvm.call @a_setattr(i64 obj, !llvm.ptr name, i64 v)
// -> i64. Phase 2: GEP+store on the unboxed struct.
def AStructSetFieldOp : A_Op<"struct.setfield"> {
  let summary = "write a struct field";
  let description = [{
    Stores `value` into field `index` of boxed struct `obj`; returns `obj`
    (the runtime may return a new boxed value for value-semantics structs).
  }];

  let arguments = (ins
    AVAL:$obj,
    I64:$index,
    AVAL:$value,
    OptionalAttr<StrAttr>:$field,
    OptionalAttr<TypeAttrOf<AStruct>>:$struct_type);
  let results = (outs AVAL:$result);

  // `struct_type` stays in the attribute dict (see a.struct.getfield).
  let assemblyFormat = [{
    $obj `[` $index `]` `=` $value attr-dict
    `:` type($obj) `,` type($index) `,` type($value)
    `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.array.get` reads element `index` of boxed array/container `array`.
// Wraps `a_getitem`.
//
// Lowering (phase 1): llvm.call @a_getitem(i64 arr, i64 idx) -> i64.
// Phase 2 (!a.array<N x T>): llvm.getelementptr + llvm.load.
def AArrayGetOp : A_Op<"array.get"> {
  let summary = "read an array/container element";
  let description = [{
    Returns element `index` of the boxed array `array`.
  }];

  let arguments = (ins
    AVAL:$array,
    AVAL:$index,
    OptionalAttr<TypeAttrOf<AArray>>:$array_type);
  let results = (outs AVAL:$result);

  // `array_type` stays in the attribute dict (see a.struct.getfield).
  let assemblyFormat = [{
    $array `[` $index `]` attr-dict
    `:` type($array) `,` type($index) `->` type(results)
  }];

  let hasVerifier = 1;
}

// `a.array.set` writes `value` at `index` of `array` and returns the
// container. Wraps `a_setitem`.
//
// Lowering (phase 1): llvm.call @a_setitem(i64 arr, i64 idx, i64 v) -> i64.
// Phase 2: llvm.getelementptr + llvm.store.
def AArraySetOp : A_Op<"array.set"> {
  let summary = "write an array/container element";
  let description = [{
    Stores `value` at `index` of boxed array `array`; returns the container.
  }];

  let arguments = (ins
    AVAL:$array,
    AVAL:$index,
    AVAL:$value,
    OptionalAttr<TypeAttrOf<AArray>>:$array_type);
  let results = (outs AVAL:$result);

  // `array_type` stays in the attribute dict (see a.struct.getfield).
  let assemblyFormat = [{
    $array `[` $index `]` `=` $value attr-dict
    `:` type($array) `,` type($index) `,` type($value)
    `->` type(results)
  }];

  let hasVerifier = 1;
}

#endif // A_OPS_TD
