//===- ATypes.td - A dialect type definitions ------------------*- tablegen -*-===//
//
// A language IR dialect types. Phase 1 models the current boxed semantics:
// every A-level value is a tagged i64 "aval" payload; !a.ptr is the opaque
// pointer used for stack slots, C-string literals and runtime ABI values.
// The parameterized types (!a.struct, !a.array, !a.enum) exist now so the
// frontend can name aggregate types; phase 2 will give them real layouts.
// Phase 2 typed scalars (!a.int<iN>, !a.float<fN>) are added as new TypeDefs
// and do not require any changes to the ops below.
//
//===----------------------------------------------------------------------===//

#ifndef A_TYPES_TD
#define A_TYPES_TD

include "A/ADialect.td"
include "mlir/IR/AttrTypeBase.td"

//===----------------------------------------------------------------------===//
// Base class for all `a` dialect types.
//===----------------------------------------------------------------------===//

class AType<string typeName, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<A_Dialect, typeName, traits> {
  let mnemonic = typeMnemonic;
}

//===----------------------------------------------------------------------===//
// AVAL: boxed A-language value (tagged i64 payload)
//===----------------------------------------------------------------------===//

// The universal A value type. In phase 1 this lowers to a plain i64 carrying
// the aval tag/payload encoding used by the C runtime (see a_runtime.h).
// All a.* value-producing ops default to this type.
def AVAL : AType<"AVAL", "aval"> {
  let summary = "boxed A-language value";
  let description = [{
    `!a.aval` is the boxed representation of every A-language value. In phase
    1 it is a single i64 word: low bits carry a tag, payload bits carry ints,
    doubles (bit-cast), bools and heap-object pointers. It lowers to `i64` in
    the LLVM dialect.

    Phase 2 will introduce unboxed scalar types (`!a.int`, `!a.float`,
    `!a.bool`) alongside this type; ops accept `AnyType` where appropriate so
    that migration does not break the op definitions.
  }];

  let genVerifyDecl = 1;

  let extraClassDeclaration = [{
    /// Returns true if `type` is a legal operand/result type for `a` value
    /// ops in phase 1 (boxed aval, plus raw machine ints used by the ABI).
    static bool isPhase1ValueType(Type type);
  }];
}

//===----------------------------------------------------------------------===//
// APtr: opaque pointer
//===----------------------------------------------------------------------===//

// Opaque pointer used for stack slots (alloca results), C-string literals and
// the `%argv` argument of `a.func` / runtime ABI values. Lowers to `!llvm.ptr`
// (ptr addrspace 0).
def APtr : AType<"APtr", "ptr"> {
  let summary = "opaque A pointer";
  let description = [{
    `!a.ptr` is an opaque pointer type. It is used for stack slots produced by
    `a.alloca`, for addresses of private string-data globals, and as the
    `%argv` parameter type of `a.func`. Lowers to `!llvm.ptr`.
  }];

  let genVerifyDecl = 1;
}

//===----------------------------------------------------------------------===//
// AStruct: nominal struct type (phase-2 payload; name only in phase 1)
//===----------------------------------------------------------------------===//

def AStruct : AType<"AStruct", "struct"> {
  let summary = "A struct type";
  let description = [{
    `!a.struct<"Name">` names a user-defined struct. In phase 1 struct values
    are still boxed `!a.aval` heap objects (runtime dict-backed); this type
    exists so signatures and metadata can refer to the nominal type. Phase 2
    will attach field layout information.
    Field names are NOT stored on this type; they are carried per-access by
    the optional `field` StrAttr on `a.struct.getfield`/`a.struct.setfield`.
  }];

  let parameters = (ins StringRefParameter<>:$name);
  let assemblyFormat = "`<` $name `>`";
  let genVerifyDecl = 1;
}

//===----------------------------------------------------------------------===//
// AArray: fixed-size array type
//===----------------------------------------------------------------------===//

def AArray : AType<"AArray", "array"> {
  let summary = "A fixed-size array type";
  let description = [{
    `!a.array<N>` is a fixed-size array of `N` elements. In phase 1 array
    values are boxed `!a.aval`; `a.array.get`/`a.array.set` lower to the
    `a_getitem`/`a_setitem` runtime calls. Phase 2 will parameterize the
    element type (`!a.array<N x T>`) and lower to LLVM array/struct loads.
  }];

  let parameters = (ins "unsigned":$size);
  let assemblyFormat = "`<` $size `>`";
  let genVerifyDecl = 1;

  let extraClassDeclaration = [{
    /// Number of elements in the array.
    unsigned getNumElements() const { return getSize(); }
  }];
}

//===----------------------------------------------------------------------===//
// AEnum: nominal enum type
//===----------------------------------------------------------------------===//

def AEnum : AType<"AEnum", "enum"> {
  let summary = "A enum type";
  let description = [{
    `!a.enum<"Name">` names a user-defined enum. Phase 1 keeps enum values
    boxed as `!a.aval`; this type exists for signatures/metadata.
  }];

  let parameters = (ins StringRefParameter<>:$name);
  let assemblyFormat = "`<` $name `>`";
  let genVerifyDecl = 1;
}

//===----------------------------------------------------------------------===//
// ANone: unit/none value type
//===----------------------------------------------------------------------===//

def ANone : AType<"ANone", "none"> {
  let summary = "A none/unit type";
  let description = [{
    `!a.none` is the type of the `none` literal and of functions that return
    no meaningful value. In phase 1 the runtime represents it as the tagged
    aval `kAvalTag | 0x2` payload, so it still lowers to `i64`.
  }];

  let genVerifyDecl = 1;
}

//===----------------------------------------------------------------------===//
// ABool: primitive boolean (phase-2 scalar; reserved now)
//===----------------------------------------------------------------------===//

def ABool : AType<"ABool", "bool"> {
  let summary = "A boolean type";
  let description = [{
    `!a.bool` is the unboxed boolean type reserved for phase 2. Phase 1 code
    keeps booleans boxed inside `!a.aval`; this type is defined now so that
    `a.if` conditions and comparisons can migrate without changing the
    dialect shape.
  }];

  let genVerifyDecl = 1;
}

#endif // A_TYPES_TD
