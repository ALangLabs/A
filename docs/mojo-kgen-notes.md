# Mojo kgen -> object implementation notes

Research notes from `modular/modular` for mirroring in the A compiler. Refs are `path:line` in the Mojo tree (`Mojo/lib/...`, `Mojo/tools/...`, `bazel/...`, `Support/lib/...`). Pipeline: parse -> LIT check -> KGEN (elaboration + opt) -> LLVM dialect -> LLVM IR -> object bytes (llc passes in-process) -> archive/`.so` via lld.

## 1. Pass pipeline orchestration (`Compiler/Pipeline/Pipeline.cpp`, `Compiler/KGENCompiler.cpp`)

Passes are `mlir::PassManager` sequences run via `Cache::cachedTransform` on an AsyncRT `CPUDevice` workqueue (`KGENCompiler.cpp:1059-1102`): `buildGenerateLibraryPipeline` then `populateElaborateModulePasses` = `buildElaborateModulePipeline` + `buildPostElaborationPipeline`. Whole-KGEN result cached; `runElaborationPipeline` (:1199) caches only up to post-elaboration for offload reuse.

`CompilationOptions` gates all (`Pipeline.cpp:117-278`): `optimizationLevel` (0=debug,1=default,>=2=aggressive); `debugLevel` (`kNoDebug`/`kLineTablesOnly`/`kFullDebugInfo`/`kSynthetic`; kNoDebug -> `DebugInfoStrip` pre-elaboration, kSynthetic -> `SynthesizeDebugInfo` post-inline); `useParametricInterpreter`, `elaborationErrorIncludePrelude`, `elaborationErrorVerbose`, `elaborationMaxDepth`, `loopUnrollingWarnThreshold`, `elaborationStats`, `fpMode.contract`, `useMLIRVerifierOnly`, `cacheBaseExtra`, `offloadOutputPrefix/Kind`.

Pipeline builders (all `Pipeline.cpp`):
- `buildCheckLITPipeline` (:27): `LowerSemanticCF`, `VerifyParameters` (non-prod), `CheckLifetimes{extendTrivialDebugLifetimes=optLevel==0}`.
- `buildLowerLITPipeline` (:44): same + `LowerLIT{dwarf lang}`.
- `buildGenerateLibraryPipeline` (:55): optional `DebugInfoStrip`; check/lower LIT; opt>=2 `RemoveUnusedParams`+`EliminateDeadSymbols`; opt>=1 nested SROA/Mem2Reg/Canonicalizer on `GeneratorOp`; `InlineParametric`{`nodebugOnly=true`, `updateDebugInfo = debugLevel != kNoDebug`}; `VerifyParameters{simplifyParameters=true}`; opt>=1 block: RemoveUnusedParams, EliminateDeadSymbols, nested SROA/Mem2Reg/SCCP/CSE, `ApplyInliner`, 2nd `InlineParametric`, `VerifyParameters`, nested SROA/Mem2Reg/Canonicalizer, RemoveUnusedParams.
- `buildElaborateModulePipeline` (:117): `EliminateDeadSymbols`; `DebugInfoStrip` on kNoDebug; opt>=1 nested `CSEPass` on GeneratorOp; `VerifyParameters`; `LiftAndFoldApply`; `ReorderParamOps(true)`; `ElaborateGenerators{target, ElaborateGeneratorsOptions, options, compileElaboratorAsm, compileOffloadFn}` with options {`elaborateDebugInfo`=lineTables||full, `optimizeInterpreter`=optLevel==0, `useParametricInterpreter`, `errorIncludePrelude`, `errorVerbose`, `maxDepth`, `loopUnrollingWarnThreshold`, `printStats`}.
- `buildPostElaborationPipeline` (:155): `SetFastMathFlags{fpMode.contract}`; `KGENVerifier{useMLIRVerifierOnly=true}`; `UpdateMaterialization` (materialized comptime values get own storage before SROA/Mem2Reg).
- `buildFirstOptPipeline` (:176): `EliminateDuplicateFunctions`; `EliminateDeadSymbols`; nested `LowerArgConventions` on FuncOp; `LowerCallingConventions`; nested `Mem2Reg`; `EnsureNoParameters` (non-prod); `InferFunctionAttrs`; `AutomaticInline` (CGSCC inliner; inner pipeline SimplifyCF/SROA/Mem2Reg/Canonicalizer/CSE/HoistTrivialInvariants/SimplifyCF/SROA/Mem2Reg/StackReuse/CSE/Canonicalizer skipped entirely opt<1; `InlinerDebugInfoUpdateTime`=Never/kDeferred/kImmediate); `SynthesizeDebugInfo` on kSynthetic; nested SROA/Mem2Reg/Canonicalizer/`RaiseForLoops`/`LoopUnrolling{optLevel}`; opt>=1 nested SROA/Mem2Reg/SCCP/Canonicalizer/CSE; `EliminateDeadSymbols`.
- `buildLateOptPipeline` (:248): opt>=1 nested RaiseForLoops/LoopUnrolling/SROA/Mem2Reg/Canonicalizer/CSE; opt>=2 same + SimplifyCF; nested `LowerLoops`; `LowerClosures`; `LowerAsyncFunctions` (deliberately late); opt>=2 `DeadArgumentElimination`; `EliminateDeadSymbols`.

Pattern: opt-level gating = adding/removing passes in builders, not inside passes. Debug policy threaded as small options structs.

## 2. LLVM dialect -> object, in-process (`Compiler/ObjectCompiler/`)

### 2a. KGEN->LLVM dialect (`ObjectCompiler/KGENToLLVMPipeline.cpp`, 63 lines)

`buildLowerToLLVMPipeline(pm, LowerToLLVMOptions)`:
1. `LowerKGENToLLVM{globalCtorFnName, globalDtorFnName}` - big pattern pass.
2. nested `KGENVerifierPass` on LLVMFuncOp.
3. `LegalizePOPOperations` (nested LLVMFuncOp).
4. `LowerGlobalPOPToLLVM` (module-level, needs SymbolTable).
5. `LowerPOPToLLVM` (nested).
6. `LowerControlFlow` (nested).
7. `ReconcileUnrealizedCasts` (nested).
8. `LowerSuspensionPoints` (nested) - async after control flow.
9. `Canonicalizer` with `GreedySimplifyRegionLevel::Disabled` (FIXME: region simplifier exponential on KGEN IR) + `CSE` (nested).
10. `createTargetSpecificLLVMLowering()`.
11. `DebugInfoToLLVM{tradeoffPerfForVariableDI = optLevel==0}`.

Registered as named pipeline `"lower-to-llvm"` via `mlir::PassPipelineRegistration` - pipelines are reified strings.

### 2b. LLVM IR translation (`ObjectCompiler.cpp`)

`lowerAllFuncsToLLVM` (from `emitArchive`, :836): MLIR pipeline -> `mlir::translateModuleToLLVMIR` (:798, names module from FileLineColLoc filename), sanitizer attrs per fn (:636-649), `linkBitcodeLibraries` (:703-796). `ExecutionEngine::getGlobalCtorFnName()`/`getGlobalDtorFnName()` feed `LowerToLLVMOptions`.

### 2c. Bitcode library linking (`ObjectCompiler.cpp:703-796`)

`backend->linkRuntimeLibraries(loc, llvmModule, options)` first - device/vendor bitcode (MLRT/AsyncRT-style) is **target-backend-owned**. Then `bitcodeLibs`: `SmallVector<pair<bool used, Attribute>>`, attr = StringAttr path or `DenseResourceElementsAttr` blob. Skip if no extern fn decls unless backend says `onlyLinkExternFunctionsInBitcodeLibs(options)` false. `llvm::Linker::linkInModule` with `LinkOnlyNeeded` when only-link-extern; else full link + `internalizeModule` of non-requested globals. Triple mismatch -> skip.

### 2d. LLVM optimization (`ObjectCompiler/LLVMPassesPipeline.cpp`, 770 lines)

`buildLLVMOptimizationPipeline(PassBuilder&, options)` (:573): lookup `TargetBackend` via `TargetBackendRegistry::get().lookup(Triple)`; backend can own pipeline (`backend->buildLLVMPipeline`). optLevel==None -> `buildO0Pipeline`, else `buildO3Pipeline` (all non-zero identical).

`buildO3Pipeline` (:370-536): backend `addPipelineStartPasses` -> `InferFunctionAttrs` -> early-FPM (LowerExpectIntrinsic, SimplifyCFG, SROA(ModifyCFG), EarlyCSE, CallSiteSplitting) -> `PromotePass` -> globalCleanup FPM (InstCombine + peepholes + SimplifyCFG convertSwitchRangeToICmp) -> `addInlinerPasses` (:267: ModuleInlinerWrapper `getInlineParamsFromOptLevel(3)`, mandatory-inlinings-first, CGSCC inliner x2, MaxDevirtIterations=4; CGSCC main = PostOrderFunctionAttrs(SkipNonRecursive) -> `buildFunctionSimplificationPipeline` -> PostOrderFunctionAttrs) -> GlobalDCE -> CGSCC-late callbacks -> ReversePostOrderFunctionAttrs -> RecomputeGlobalsAA -> optimize-FPM (Float2Int, LowerConstantIntrinsics, loop rotate+deletion, LoopDistribute, InjectTLIMappings, `addVectorPasses` = LoopLoadElimination, SimplifyCFG switch->lookup-table, SLPVectorizer, VectorCombine, SROA(PreserveCFG), LICM-spec; LoopSink, InstSimplify, DivRemPairs, TailCallElim, SimplifyCFG, InstCombine) in `createModuleToFunctionPassAdaptor` -> optimizer-last callbacks -> `addSanitizers` -> GlobalDCE, ConstantMerge, CGProfile, RelLookupTableConverter, AnnotationRemarks.

`buildFunctionSimplificationPipeline` (:122-265) CGSCC body: SROA, EarlyCSE(memssa), SpeculativeExecution(divergent-only), JumpThreading, CorrelatedValuePropagation, SimplifyCFG, InstCombine, AggressiveInstCombine, ConstraintElimination, LibCallsShrinkWrap, TailCallElim, Reassociate, lpm1 (LoopInstSimplify/LoopSimplifyCFG/LICM-nospec/LoopRotate/LICM-spec/SimpleLoopUnswitch) + lpm2 (LoopIdiomRecognize/IndVarSimplify/LoopDeletion), then SROA, VectorCombine(earlyFolds), MergedLoadStoreMotion, GVN, SCCP, BDCE, InstCombine, JumpThreading, CVP, ADCE, MemCpyOpt, DSE, LICM-spec(MemorySSA), SimplifyCFG(hoist/sinkCommonInsts), InstCombine.

`buildO0Pipeline` (:538): backend pipeline-start, O0 EP callbacks, `AlwaysInlinerPass(InsertLifetimeIntrinsics=false)`, CGSCC-late, optimizer-last, sanitizers, AnnotationRemarks.

Extension points: PassBuilder EP callbacks at pipeline boundaries (`invokePipelineStartEPCallbacks`, `invokePipelineEarlySimplificationEPCallbacks`, `invokeOptimizerLastEPCallbacks`, CGSCC late).

### 2e. TargetMachine + MC emission (`ObjectCompiler.cpp`)

`createTargetMachine(options, isJIT)` (:614): `TargetBackendRegistry::lookup` -> `backend->adjustOptionsForTargetMachine` -> `defaultCreateTargetMachine`. Triple/cpu/features from `CompilationOptions` via `parseTargetOptions` (`Compilation.cpp`) -> `TargetInfoAttr`.

`runLlcPasses` (:279-358): legacy `PassManager` + `TargetLibraryInfoWrapperPass` + `RuntimeLibraryInfoWrapper(ABIName, VecLib)`; non-prod `verifyModule`; `addPassesToEmitMC(..., stopBeforeAsmPrint=true, numFunctionsBase)` (object path - MC merged across splits later) or `addPassesToEmitFile` (asm); `ObjFileLowering->initialize(MCContext, TM, module)`.

`emitArchive` (:1027-1180): cached via `cachedTransform` keyed by `options.print("emitArchive(isJIT=, ...)")`; inside: lower -> link bitcode -> parallel split compile -> MCLink -> BufferRef. `emitArchiveParallelCompilation` (:862): `noSplitting` if parallelism<2; `parLLC` = parallelism>=2 && `enableParallelLLC` && `!backend.isCodegenInterprocedural()`; `SplitStrategy` from `backend.splitStrategy(options)`: None->single, PerFunction->`splitPerFunction`, else `splitPerExported(..., outlineSharedSubtrees = !backend.isOffload(), parallelism)`. `emitArchiveMCLinking` (:907) -> `MCLinker` merges per-split MC; `seenCodeGenFns` dedups fn names.

`compileOptimizedLLVMModuleToObject` (:362-508): runs on `cpuDevice.getWorkQueue()`; par-LLC bitcode-serializes module into cache key, reloads into fresh `LLVMContext` per task (avoids AnnotationCache races); per-task `createTargetMachine`; sets `MCOptions`; `runLlcPasses` -> `MCInfo{WriteableBufferRef, MachineModuleInfo, LLVMModuleAndContext, fnNameToFnPtr, TargetMachine, MCContext, splitIdx}`; `resetSubtargetInfo` into shared TM under mutex.

`emitSharedObject` (:1448) ELF/MachO only: writes `.o`, invokes `ld.lld -shared` (ELF `-flavor gnu`) or `ld64.lld -flavor darwin -platform_version macos ... -arch arm64|x86_64 -undefined dynamic_lookup -dylib`; backend `appendLinkArgs` inserts runtime link flags.

`SyncX86SymbolTables` (LLVMPassesPipeline.cpp:695-743): MachineFunctionPass syncing external MCSymbols across per-split MCContexts for ORCJIT; added for x86 triples in `addPassesToAsmPrint` (:747). Kernel path cache key adds `kgen.offload.kernelid` fn attr (:1486-1515); `parseEmissionOptions`/`resetEmissionOptions` (KGENCompiler.cpp:1215+) set arbitrary LLVM cl::Option values via `addOccurrence` (`-mllvm`-style flags through `--X-emit-options`).

### 2f. TargetBackend interface (`include/Mojo/Compiler/Target/TargetBackend.h`)

`TargetBackendRegistry` singleton: `lookup(triple)` errors on accelerator-without-MAX; `RegisterTargetBackend<T>` static-init registrar (:366). `TargetBackend` hooks: `traits()`, `name()`, `matches(triple)`, `resolve(triple)` (:145-170); `preprocessGenerator`, `splitStrategy` (:172-189), `SplitStrategy {None, PerExported, PerFunction}` (:98); `isCodegenInterprocedural()` (:189), `isOffload()` (:193), `isBaseTarget()=0` (:212), `requiresOriginalFunctionOrder()` (:218), `requiresSingleModuleSplit()` (:223), `reorderLinkedModuleFunctions` (:228); `adjustSimplifyCFGOptions` (:235, GPUs raise branch threshold), `addSanitizers` (:244), `adjustOptionsForTargetMachine` (:255), `finalizeModuleForTarget` (:261), `emitBitcode` (:268); `buildLLVMPipeline` (:275, backend may own whole opt pipeline), `addPipelineStartPasses` (:283), `registerPipelinePasses` (:289); `onlyLinkExternFunctionsInBitcodeLibs` (:304), `attachCodegenAttributes` (:310), `appendLinkArgs` (:313), `ownsOffloadLowering` (:319), `emitAssembly`/`emitObject` (:330-335). `EmitContext`/`OffloadEmitContext` (:101-138) carry RunLlc/LinkObject callbacks so offload reuses host plumbing.

## 3. KGEN->LLVM lowering (`KGENToLLVM/`)

### 3a. Type converter (`KGENToLLVM/LLVMLoweringUtils.h`, `.cpp:304-524`)

`LLVMDataLayout` wraps `TargetInfoAttr` data layout: `getTypeSizeInBits/StoreSize/AllocSize/ABIAlign`, `getStructFieldOffsets`. `POPToLLVMTypeConverter : mlir::LLVMTypeConverter` caches `structFieldIndexCache`. Conversions:
- `KGEN::NoneType`/`NeverType` -> empty literal struct
- `StringType` -> `{ptr, index}` struct
- `FuncType` -> `convertType(getValues())`; `FuncTypeGeneratorType` -> body
- `PointerType` -> `LLVMPointerType` w/ addressSpace attr
- `POP::ArrayType` -> `LLVMArrayType` (needs resolved size)
- `StructType` -> literal struct **with explicit i8 padding fields inserted where KGEN offsets exceed LLVM-natural offsets** - uses `DataLayoutInterface::getTypeAllocSize/ABIAlign` on KGEN types consistently, caches logical->LLVM field index (`getRemappedFieldIndex`)
- `SIMDType` -> `VectorType` (scalar if size==1)
- `DTypeType` -> i8; `CO::CoroutineType` -> opaque pointer
- `POP::UnionType` -> `{repr, tail}` struct (repr for alignment, tail array for payload; tail element capped at pointer width)

`InterpreterMemoryConverter` dedups comptime memory blobs -> SSA ptrs or LLVM globals for `const_global`; `MaterializationScope` = identity scoping. `DebugInfoTypeConverter : DebugInfo::DebugInfoTypeConverter` - per-KGEN-type `buildDebugType` overloads; `getError()`. `ConvertPOPToLLVMPattern<OpT>` = `DirectOpToLLVMPattern` - eager single-visit rewrite (not greedy); `getTypeConverter()` -> `POPToLLVMTypeConverter*`.

### 3b. Pass driver (`KGENToLLVM/LowerKGENToLLVM.cpp:1372-1474`)

`LowerKGENToLLVMPass::runOnOperation`: `lookupTargetInfo(module)` -> `POPToLLVMTypeConverter(targetInfo)`; sets `llvm.target_triple`/`llvm.data_layout` module attrs, erases `EnvAttr`; emits `LLVM::ModuleFlagsOp` `"target-abi"` (ModFlagBehavior::Error) when `targetInfo.getAbi()` non-empty; `populateKGENToLLVMPatterns(typeConverter, patterns, symtab, imc, passFailed, ids, cabiDefinitions)` - pattern list :1317-1338 (`ConvertKGENCall`, `ConvertKGENFunc`, `ConvertKGENReturn`, `ConvertKGENParamConstant`, `ConvertKGENParamMaterialize`); `DebugInfo::populateTypeConversionPatterns`; `mustConvert` whitelist (HLCF Return/Unreachable always, KGEN dialect ops except `StructInstanceOp`, debug ops only if `!typeConverter.isLegal`); `M::applyDirectConversion` (eager); per-`LLVMFuncOp` `TargetLowering::finalizeConvertedFunction`; `debugTypeConverter.applyRecursively`; erase `StructInstanceOp`s; `signalPassFailure` on error. `getLinkageKind`: NotExported->Internal, Exported->External. Generated via `#define GEN_PASS_DEF_LOWERKGENTOLLVM` + `#include "Mojo/KGENPasses.h.inc"` from `include/Mojo/KGENPasses.td`.

### 3c. Target dispatch (`Target/TargetLowering.h`, 385 lines)

`TargetLowering` virtual hooks: `traits()`, `name()`, `matches(triple)`, `resolve(triple)`, `lowerFunctionLocalPOPToLLVM`, `populateLowerGlobalPOPToLLVMPatterns`, `addPostLowerToLLVMPasses`, `isLoweredInGlobalPOPPass`, `isConvergentOp`, `markExportedKernel`/`isExportedKernel`/`getKernelByValArgAttrName`/`lowerKernelArgToMemory`/`getKernelArgIndirectionType`, `needsVerification`/`verifyOp`, `mapKernelArgMetadata`, `finalizeConvertedFunction`, `mapFuncMetadata`, `populateLegalConversions`, `typeNeedsExtraLegalization`, `castHandledByLaterPass`, `isBaseTarget()` (pure - gates MAX-only targets), `supportsDType`, `getDTypeDebugName`, `buildDebugTypeForDType`. `TargetLoweringRegistry` singleton: `add`, `lookup(triple)`; `RegisterTargetLowering<T>` static registrar. Host impl: `KGENToLLVM/Target/Host/HostLowering.cpp` (38 lines, `isBaseTarget()=true`). GPU targets under `KGENToLLVM/Target/max_license/` (NVPTX/AMDGPULowering/Metal/Plugin/GPUCommon).

### 3d. C ABI lowering (`KGENToLLVM/CABILowering.h/.cpp`)

`enum class ABIArgClass {Integer, SSE, IntegerPair, SSEPair, Mixed, Memory, NoClass}`; `enum class IntExtKind {None, ZExt, SExt}`. `struct CoercionInfo` {argClass, coercedType, coercedSecondType, useIndirect, useByval, useSRet, ext; factories `indirectArgument(useByval)`, `sretReturn()`; `isIdentity()`, `isTwoRegister()`, `getNumLLVMParams()`, `applySignedness(bool)` ZExt->SExt}. `struct SignatureClassification` {args, ret; `getLLVMParamIndex(argIdx)` (sRet shifts by 1); `getFunctionType(ctx,argTypes,retType,isVarArg,numFixedArgs)`; `getParamAttrs` (moves arg attrs to first LLVM param, adds sret/byval/zeroext/signext, strips `noundef` on by-value structs); `getResultAttrs`}. `struct CABIRole` {isCaller, isArgSigned[], isResultSigned; `caller(isArgSigned)`/`callee(isResultSigned)`} - caller strips ext on return, callee strips ext on args (C can't be assumed to extend). `class CABIInfo` {`computeSignatureInfo(argTypes,retType,loc,role,numFixedArgs=SIZE_MAX)`; virtual `classifySignature` (order-dependent override, e.g. SysV rollback-to-stack), pure `classifyArgumentType`/`classifyReturnType`}. Subclasses: `DefaultCABIInfo` (identity), `SystemVABIInfo` (`CABISystemV.cpp`, 315 lines), `AAPCSABIInfo` (`CABIAAPCS.cpp`, ARM64 incl. HFA <=4 rule); Win64 noted future. `createCABIInfo(triple, ctx, dataLayout)` factory. `CabiUtils`: `getStructSize`, `isAllFloatStruct`, `forEachMember`, `getIntegerTypeForSize`, `classifySmallIntegerStruct`, `classifyExtendedScalar`, `isCABIEnabled` (default on; `--skip-c-abi-coercion` debug flag). `CABICallHelpers` lowers `pop.external_call` -> `LLVM::CallOp` with C ABI struct coercion - callee compiled by clang expects coerced signature.

### 3e. Convention lowering passes (`Transforms/`)

`LowerArgConventions.cpp` (981 lines): `lowerPointerType` promotes `PointerType` args to element type unless memory-only struct or `target->lowerKernelArgToMemory(elType)`; `enum ABI {DontPromote, PromoteResult, PromoteError, PromoteBoth, RemoveError, PromoteResultRemoveError}`; `runOnOperation` lowers FuncOp signature then walks `kgen.call`/`kgen.call_indirect`/`pop.external_call`, expands variadic pack operands on external_call into individual args; second pass `AttrTypeReplacer` rewrites all FuncType/TypeParamAttr in attrs/locs/types; `maxInlineSize` option; `isCABI` on signature delegates struct lowering to platform ABI classifier. `LowerCallingConventions.cpp`: removes `!kgen.none` result types, sets `fnEffects.setThrows(false)`, `ByRefError`->`ImmReg`, lowers param packs to structs (`lowerPackStructType` strips `isParamPack` bit), `recursiveRewrite` over regions.

Other transforms in `Transforms/` (36 .cpp): ArgPromotion, AutomaticInline, DeadArgumentElimination, EliminateDeadSymbols, EliminateDuplicateFunctions, EnsureNoParameters, ForceAlwaysInline, HoistTrivialInvariants, InferFunctionAttrs, InlineParametric, LowerAsyncFunctions, LowerClosures, LowerLoops, RaiseForLoops, RemoveUnusedParams, SROA, Mem2Reg, SCCP, StackReuse, VerifyParameters, LegalizePOPOperations, LowerPOPToLLVM(+ExternalCalls), LowerControlFlow, LowerSuspensionPoints, SetFastMathFlags, TargetSpecificLLVMLowering, CheckLifetimes, LiftAndFoldApply, ReorderParamOps, SynthesizeDebugInfo, DebugInfoStrip...

## 4. `.mojoc` bytecode packages & lazy loading (`MojoParser/`, `Support/`)

### 4a. File classification (`Support/lib/Filesystem/Paths.cpp`)

`isMojoSourcePackagePath` = dir containing `__init__.mojo`; `isMojoBinaryPackagePath` = extension `.mojoc` (comment mentions `.mojopkg` but only `.mojoc` checked, :37); `isMojoSourceFile` = `.mojo`.

### 4b. Package format (`Support/MojoPrecompiledFile.cpp`)

Magic `"MPKG"` + 1-byte `MojoPrecompiledFileFormatVersion` (V2 adds zstd) + 3 reserved + `MojoPrecompiledFileVersion` (mojo u8 major/u8 minor/u16 patch/NUL label; then max/modular version same layout) + NUL `mlirChecksum` + (V2) u64 uncompressed size + pad to 8 + zstd MLIR bytecode. `isMojoPrecompiledFile` checks `"MPKG"` prefix; `getMLIRBufferFromPrecompiledFile` decompresses and `checkCompatiblePrecompiledFile` unless `MOJO_NO_VALIDATE_MOJOPKG` env or `ignoreIncompatiblePrecompiledFileErrors` option. Compatibility = `header.mlirChecksum == getMojoMlirDialectChecksum()`, else version error ("recreate with `mojo precompile`").

### 4c. Import resolution (`MojoParser/ModuleLoader.cpp`, 1505 lines)

`ModuleSpec::classify` -> `SourcePackage`/`SourceDir`/`Precompiled`/`SourceModule`; `takesImportPrecedence` ordering; `traverseImportDirectories` order: auto-import dirs (`-I` `searchPaths` comma-split + `extraSearchPaths`, else `MojoConfig::getParserImportPaths`), then working dir derived from **main buffer** (`deriveWorkingDirectory` - never importing file's dir), then `sourceMgr.getIncludeDirs()`. `.mojoc` skipped when `arePrebuiltPackagesDisabled()` or inside source package. Namespaces = plain dirs spanning multiple roots; ambiguity -> error; closed candidates beat directory portions. `kMovedStdlibSubpackages = {"runtime","gpu","algorithm"}` + migration note.

### 4d. Binary package materialization (`ModuleLoader.cpp:1136-1316`)

`createBinaryPackageState`: top-level import only (nested -> error); binding aliases via `originsByCanonicalPath` (weakly_canonical); file -> `getMLIRBufferFromPrecompiledFile` -> own `std::shared_ptr<llvm::SourceMgr>` (bytecodeReader holds ref) -> `mlir::BytecodeReader(buffer, parserConfig, /*lazyLoad=*/true, sourceMgr)` -> `readTopLevel(block)` -> `materialize(tmpModule)`; extract `PackageOp` to parent; iterate `FnOp` stubs -> `shared.tryRegisterConversionThunk(key=thunk.getThunkKeyAttr(), thunk)` dedup into `SharedState::Impl::conversionThunks` (`DenseMap<Attribute,FnOp>`, key = ArrayAttr [actual sig, thunk sig]) -> `bytecodeReader->materialize(thunk)` -> top-level -> `declResolver->addBytecodeDecl(&*thunk, thunk.getSourceNameAttr(), &topLevelDecl, DeclResolvedness::body)` + `finalizeFuncSignature`; `StructDeclOp`s with `closureThunkKeyAttr` -> `getOrCreateClosureDeviceType`/`getOrCreateParamClosureExtension`/`getOrCreateInflatedClosure` (`kClosureDeviceTypeSuffix`, `kClosureExtensionPrefix`); `PackageASTDecl` with `loadedFromBytecode=true`, `resolvedness=signature`; stores `origin.bytecodeReader`, `origin.sourceMgr`, `origin.tmpModule`, `origin.bytecodeImportLoc` on `ModuleOrigin`. Synthesizes universal parametric closure trait if referenced (`UNI_CLOSURE_TRAIT_NAME`).

`createArtifactChildState` (:1318-1340): nested FileModuleOp/PackageOp -> `addBytecodeDecl(op, /*unlisted*/StringAttr(), parent, DeclResolvedness::unparsed)` + `ModuleState` sharing `parentState.origin`.

### 4e. Lazy decl resolution (`SharedState.cpp`, 3016 lines)

`DeclResolvedness`: unparsed < signature < body. `resolveDeclFromBytecode` (:1496-1768): signature phase walks attr dict resolving `FuncSymbolAttr`/`StructMetaType`/`TraitType`/`StructType` refs via `BytecodeResolutionReferenceWalker` (`SymbolRefAttr` -> `resolveRefParentDecl` -> `lookupAndResolveMangledDecl` - `SymbolTable` lookup, `extension:`-prefixed names, FileModuleOp/PackageOp -> `ModuleState::nestedModules`, else `container.lookupInCurrentScope`; first call sets `referencedFromBytecode` and resolves all children signatures). Body phase: find `bytecodeReader` via parents -> `origin->bytecodeReader`; `isMaterializable(op)`/`materialize(op)`; `symbolTables.invalidateSymbolTable` after materialize; container ops recurse (`PackageOp.getDependenciesAttr()` imports each dep); region ops -> `addDeclForOp` per kind (FnOp->DefASTDecl, ImportOp variants, StructDeclOp->StructASTDecl+param decls via `demangleParameterName`, TraitDeclOp, ExtensionDeclOp->`registerExtensionDecl`, AliasDeclOp, StructFieldOp, FileModuleOp/PackageOp->`createArtifactChildState`, ConformanceOp->witness tables signature-only via `walkSignaturesOnly`); default ops -> `deferredOps`/`resolveSingleOp`; ConformanceOp special-cased to avoid recursive body materialization.

`finalizeImportedBytecodeModules` (:1770-1818): erases unparsed decls whose ops materialized; `bytecodeReader->finalize([&](op){ return !op->use_empty(); })` - keeps used ops; erases `origin->tmpModule`.

Other SharedState facts: `Impl` maps `conversionThunks`, `paramClosureExtensions`, `inflatedClosureStructs`, `closureDeviceTypeStructs`; `capturesInScope`/`captureConventionForScope`; caches `cachedImplicitConvertibility`, `nominalConformanceCache`, `builtinTraitCache`/`builtinTypeDeclCache` (key = `makeBuiltinCacheKey` = relativeLevel dots + components + name); `lookupAndResolveDecl` resolves signatures lazily, expands wildcard imports on miss; `importBuiltinModules` imports `std` + wildcard `std.prelude` into every module; `materializeDeferredModule` opens deferred source on first body resolution; incremental cache: `contentHash = DepGraph::hashModuleBuffer`, `kIncrParsingCacheResumeInfoAttrName` resume attrs (`needsSourceResumeMark` by DeclResolvedness), `buildDepGraph` emits `depgraph.module` nodes/edges.

### 4f. Entry point (`MojoParser/EntryPoint.cpp`, 539 lines)

`importMojoImpl` (:295), `importMojoFileImpl` (:368), `LIT::importMojoPackage` (:408, runs `eraseUnreachableDecls` :135 + `sortValueUses` :57-133), `importMojoFile` (:454), `cloneDeclModuleForCompilation` (:464/:468). `ParserListener` (:511-537) no-op hook interface for tooling (aliases, args, fns, imports, member lookups, params, structs, traits, vars, refs, calls, param bindings, match-missing-cases).

## 5. Elaborator (`Elaborator/`, ~1 page)

`struct ExpansionGraph` (`Elaborator/Elaborator.h`): `nodes: Shared<MapVector<pair<ParameterExprArrayAttr, GeneratorOpInterface>, unique_ptr<ParamNode>>>` - dedup key = (param values, generator); `std::atomic<size_t> numWorkItems=1`; `worklistCh` AsyncValueRef<Chain> signalled when all work items done (starves workqueue before evaluators since evaluation can't run concurrently with compiler work); `elaboratedNodes: vector<unique_ptr<ImplNode>>`; `numOutstandingResources`; `quiesceChain`; `eraseNodeInst`; `didAddTask`/`didCompleteTask`.

`class Elaborator : public InterpreterCache`: `symtab`, `paramCache` (`ParameterCollector::Analysis`, thread-local caches sized by workqueue parallelism), `target`, `options`, `compileAsmFn`/`compileOffloadFn`, config; `concretizeSymbolsWithin`, `lookupInstantiatedOp`/`lookupImplNode` (`tlInsts`/`concreteNodes` Shared maps), `getConcreteFunction`, `concretizeCallee(parent,node,loc)` (readiness/error protocol), `specializeGenerator`, `getOrCreateNode(ParameterExprArrayAttr values,...)`, `calculate(paramCache.getThreadLocalCache())`.

`IREvaluator.h`: `ImplNodeBase` {instantiated op, ParameterUseDefGraph, evaluator (ParameterEvaluator for param substitution), `parent: ImplNode*`}. `ImplNode` {`ParamNode *parent`, `std::vector<WorkItem> stack` WorkItem={ops, onComplete(ImplNode*), per-scope IREvaluator}, `dependencies` (deferred generator instantiations via calls), `sccRemovedDeps` (intra-SCC edges removed by `diagnoseAndBreakRecursion`, checked in second pass `completeImplNodeProcessing`), `blocker` optional{Location,ParamNode*}}. `ParamNode` {`ImplNode impl`, `ExpansionGraph *expansionGraph`, `andThenAsync(AsyncValue::Waiter)`, `emplace()`, `setToError()`; ctors: (cpuDevice, GeneratorOpInterface, ParameterExprArrayAttr, depth, graph) or root `ParamNode()`}. `ParamNodeBase` {cpuDevice, gen, vals, depth, `lookupParamNodeBase`, `getGenerator`, `compileAsm`, `addDeferredFunction`, `getParentNode`, `evaluator`, `elaborator`, `parent`}.

`InterpreterCache.h`: `ConcreteFunction{Region*, shared_ptr<CompiledRegion>}` lazily compiles `FunctionIRBytecode` once (`compileIfNecessary(region,target,optimize)`, atomic flag, optional clone FuncOp when optimizing); `InterpreterCache : BytecodeCompiler` {`Shared<DenseMap<Region*, ConcreteFunction>> cache` + `mlir::ThreadLocalCache` `tlc`; `addRegion`, `compileBytecode(Region&)`}; `Elaborator` IS-A InterpreterCache; `optimizeInterpreter` -> clone+optimize before bytecode gen (set optLevel==0).

`Elaborator.cpp` (3210 lines): error propagation via atomic state check (:1534-1547, single-threaded DFS :1780, multi-threaded deferred call resolution :1812-1814 "maximizes parallelism on the expansion graph (without intra-node parallelism)"); `interpretWithCache` for comptime eval (:858-889, :1309-1328); `ParametricElaborator` when `options.useParametricInterpreter` (:3169-3183). Parallelism = AsyncRT CPUDevice workqueue tasks; `Shared<>`/`ThreadLocalCache` for thread-safe maps; node cloning remaps ops so callbacks access through node not captured handles.

## 6. Build system (`bazel/internal/tablegen.bzl`, `Mojo/BUILD.bazel`)

`td_library` = mlir `td_library` wrapper applying package overlay (`overlay_srcs`/`overlay_includes`).

`gentbl_mlir_dialect(name, dialect, td_file, extra_tbl_outs=[], deps=[])` (tablegen.bzl:170-223) "mirrors add_mlir_dialect from cmake": `mlir-tblgen` 6x -> `<td>.h.inc` (`-gen-op-decls`), `.cpp.inc` (`-gen-op-defs`), `Types.h.inc`/`Types.cpp.inc` (`-gen-typedef-decls/-defs -typedefs-dialect=<d>`), `Dialect.h.inc`/`Dialect.cpp.inc` (`-gen-dialect-decls/-defs -dialect=<d>`), optional `extra_tbl_outs` e.g. `-gen-bytecode -bytecode-dialect=<d>` -> `*DialectBytecode.cpp.inc`; plus `gentbl_filegroup` `-gen-dialect-doc -dialect=<d>` -> `<X>Dialect.md`. CMake equivalent: `mlir_tablegen` custom commands or `add_mlir_dialect()` from MLIRConfig.

`gentbl_modular_passes(name, td_file, output_name=None, deps=[])` (:225-259): `mlir-tblgen -gen-pass-decls [-name <out>]` -> `<td>.h.inc` included as `Mojo/KGENPasses.h.inc` (pass bases `KGEN::impl::<Pass>Base`); `.md` doc via `-gen-pass-doc`. TD example `include/Mojo/KGENPasses.td`: `include "mlir/Pass/PassBase.td"`, `def Canonicalizer : Pass<"canonicalizer">`, passes per-dialect `.td`.

`driver_option_tablegen` (:60-168): `llvm-tblgen -gen-opt-parser-defs` -> `.inc`; custom `//Support/tools/driver-tblgen -gen-help-texts -gen-man-page -gen-markdown`; produces `OptTable.inc` + `*HelpTexts.inc` consumed via `#define DRIVER_OPTIONS_PATH`/`#include "DriverOptionsHelpTexts.inc"` + `#include "Support/Driver/OptTable.inc"` in mojo.cpp:52-54.

`Mojo/BUILD.bazel` per-dialect pattern: `td_library(<D>DialectTdFiles, glob .td, deps=[other TdFiles, //Support:MTdFiles, BuiltinDialectTdFiles, ControlFlowInterfacesTdFiles, ...])` -> `gentbl_mlir_dialect` -> optional `gentbl_cc_library` for `-gen-bytecode` -> `modular_cc_library` globbing `lib/<D>/*.cpp` + `include/Mojo/<D>/*.h`. Dialects: CODialect, DepGraphDialect, KGENDialect, Interpreter, HLCF, LIT, POP, MDialect (Support). `mlir_nanobind` for python bindings (max-only).

## 7. `mojo` driver & compilation-server (`Mojo/tools/`)

`tools/mojo/mojo.cpp` `main` (:60-242): `forceLinkMLIRC()`, `registerCommandFlags`, `InitLLVM`, `installOOMHandler`; `SubcommandRegistry` build/demangle/doc/format/precompile/repl/debug/run; empty args->repl; first arg via `options::Table` (`OptTable.inc`): `--version`, `--print-cache-location`, `--clear-cache [-f]`, `--help`; `OPT_INPUT`->subcommand callback, `.mojo` suffix->`run`, else error; unknown option->`run`.

`tools/mojo/Common/Compilation.cpp`: `parseCommonMojoArguments` (:46-130) expands marshalled options into `DriverOptions`/`FrontendOptions`/`CompilationOptions`/`TargetArgs`/`MarshallingErrors`; `--print-supported-targets/cpus/accelerators`, `--print-effective-target`; `openMojoInputFile`->SourceMgr; `initializeCompilation(ctx)` registers all KGEN dialects + `registerKGENToLLVMTranslation` + `MDialect`; `validateCompilationOptions`; `parseTargetOptions`->`TargetInfoAttr`; `--lld-path`->`MojoConfig::setLLDPathOverride`. `invokeMojoParser` (:371-483): fix-it modes, register dialects, `configureKGENContext`, parse->`ModuleOp`; collects `LLVMBitcodeLibArrayAttr` from `-bitcode-lib` + `LIT::PackageOp.getExternLLVMBitcodeModulesAttr()` walk -> module attr; tags module with `EnvAttr` from `parseDefinesWithDefaults`. Timing: `MLIRPassTiming` (`--mlir-timing`, `--timing-file`, JSON `TimingReportSink`), `LLVMPassTiming` (`--llvm-timing`, forces `numThreads=1`), `ElaborationStatsReport`.

`mojo run` (`Run/mojo-run.cpp` ~150-260): `KGENCompiler(context, options, pmOptions).runKGENPipeline(*module, target)` -> check `main` in `getExportedSymbols` -> `ObjectCompiler::create(kMojoCacheBaseDirName, options, isJIT=true, context, pmOptions)` -> `LLVMBitcodeLibArrayAttr::externalize(objCompiler.getBitcodeLibs())` -> `emitArchive(module)` -> `initializeExecutionEngine(..., isJIT=true)` -> `engine.addIfAbsent<StaticArchiveLayer>("exec", archive)` -> `engine.lookup("main")` -> `executeMain` via `engine.runProgram("exec","main",fn)` under `llvm::CrashRecoveryContext`. `mojo run` = in-process JIT: KGEN->LLVM IR->object bytes->ORCJIT StaticArchiveLayer.

`mojo build` (`Build/mojo-build.cpp`): `compileModuleToArchive` (:193+) same KGENCompiler+ObjectCompiler path -> archive, sets `offloadOutputPrefix` for `compileOffloads()`; link step (:336-617) uses **system C compiler as linker** (non-Windows), `MojoConfig` `getCompilerRTPath()` (compiler-rt), TSan variants `getRuntimeGlobalsTSanPath`/`getCompilerRTTSanPath`, Mach-O `xcrun dsymutil`, VCRT matching ~:527, `-lm`/expm1 ~:562. Product kinds from target traits.

compilation-server (`tools/compilation-server/`): standalone server over `LLVMServer` transport (`MessageHandler`/`JSONTransport`); methods `initialize`/`initialized`/`shutdown`/`emitArchive`/`echoMLIR`; `LLVMServer::emitArchive` parses MLIR text -> `ObjectCompiler::create(kMojoCacheBaseDirName, params.compilationOptions, params.isJIT, mlirCtx)` -> `emitArchive` -> base64 archive. Out-of-process/offload compile requests carry serialized `CompilationOptions` + `isJIT`.

## Takeaways for the A toolchain

- Cache every pipeline stage as `cachedTransform` keyed on serialized options; keep pass pipelines as named reified strings.
- Split MLIR->LLVM dialect lowering (patterns + target hooks) from LLVM IR opt (PassBuilder + backend hooks) from MC emission (split/merge, lld); mirror `TargetBackend`/`TargetLowering` registries.
- `.mojoc`-style packages: version header + dialect checksum + zstd bytecode + lazy `BytecodeReader` + per-decl materialization with symbol-table invalidation; dedup thunks by signature-pair key.
- Elaborator: expansion-graph dedup keyed on (params, generator), work items on AsyncRT workqueue, interpreter cache with lazy per-region compile.
- CMake equivalents: `gentbl_mlir_dialect` -> `mlir_tablegen` custom commands (6 invocations per dialect + optional bytecode/doc); `gentbl_modular_passes` -> `mlir-tblgen -gen-pass-decls/-gen-pass-doc`; driver options -> `llvm-tblgen -gen-opt-parser-defs` + help-text generator.
