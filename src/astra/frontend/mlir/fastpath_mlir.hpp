// Auto-generated from src/astra/runtime/fastpath.c semantics: MLIR llvm.func
// definitions for the a_*_fast exports so the LLVM inliner can fold the
// NaN-box tag checks into callers. Regenerate with the fastgen script if
// fastpath.c semantics change. linkonce_odr keeps these discardable and
// conflict-free with libaruntime.a's fastpath.o.
#pragma once

namespace a {
namespace fastpathmlir {

inline constexpr const char kFastPathMLIR[] = R"MLIR(llvm.func @a_add_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.add %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v71 = llvm.icmp "sge" %v69, %v70 : i64
  %v72 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v73 = llvm.icmp "slt" %v69, %v72 : i64
  %v74 = llvm.and %v71, %v73 : i1
  llvm.cond_br %v74, ^L5, ^L3
^L5:
  %v75 = llvm.mlir.constant(1 : i64) : i64
  %v76 = llvm.shl %v69, %v75 : i64
  %v77 = llvm.mlir.constant(1 : i64) : i64
  %v78 = llvm.or %v76, %v77 : i64
  %v79 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v80 = llvm.and %v78, %v79 : i64
  %v81 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v82 = llvm.or %v80, %v81 : i64
  llvm.br ^L4(%v82: i64)
^L2:
  %v83 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v84 = llvm.and %a0, %v83 : i64
  %v85 = llvm.icmp "eq" %v84, %v83 : i64
  %v86 = llvm.mlir.constant(1 : i64) : i64
  %v87 = llvm.and %a0, %v86 : i64
  %v88 = llvm.mlir.constant(0 : i64) : i64
  %v89 = llvm.icmp "ne" %v87, %v88 : i64
  %v90 = llvm.and %v85, %v89 : i1
  %v91 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v92 = llvm.and %a0, %v91 : i64
  %v93 = llvm.mlir.constant(1 : i64) : i64
  %v94 = llvm.lshr %v92, %v93 : i64
  %v95 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v96 = llvm.and %v94, %v95 : i64
  %v97 = llvm.mlir.constant(0 : i64) : i64
  %v98 = llvm.icmp "ne" %v96, %v97 : i64
  %v99 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v100 = llvm.or %v94, %v99 : i64
  %v101 = llvm.select %v98, %v100, %v94 : i1, i64
  %v102 = llvm.sitofp %v101 : i64 to f64
  %v103 = llvm.bitcast %a0 : i64 to f64
  %v104 = llvm.select %v90, %v102, %v103 : i1, f64
  %v105 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v106 = llvm.and %a1, %v105 : i64
  %v107 = llvm.icmp "eq" %v106, %v105 : i64
  %v108 = llvm.mlir.constant(1 : i64) : i64
  %v109 = llvm.and %a1, %v108 : i64
  %v110 = llvm.mlir.constant(0 : i64) : i64
  %v111 = llvm.icmp "ne" %v109, %v110 : i64
  %v112 = llvm.and %v107, %v111 : i1
  %v113 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v114 = llvm.and %a1, %v113 : i64
  %v115 = llvm.mlir.constant(1 : i64) : i64
  %v116 = llvm.lshr %v114, %v115 : i64
  %v117 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v118 = llvm.and %v116, %v117 : i64
  %v119 = llvm.mlir.constant(0 : i64) : i64
  %v120 = llvm.icmp "ne" %v118, %v119 : i64
  %v121 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v122 = llvm.or %v116, %v121 : i64
  %v123 = llvm.select %v120, %v122, %v116 : i1, i64
  %v124 = llvm.sitofp %v123 : i64 to f64
  %v125 = llvm.bitcast %a1 : i64 to f64
  %v126 = llvm.select %v112, %v124, %v125 : i1, f64
  %v127 = llvm.fadd %v104, %v126 : f64
  %v128 = llvm.fcmp "uno" %v127, %v127 : f64
  %v129 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v130 = llvm.bitcast %v129 : i64 to f64
  %v131 = llvm.select %v128, %v130, %v127 : i1, f64
  %v132 = llvm.bitcast %v131 : f64 to i64
  llvm.br ^L4(%v132: i64)
^L3:
  %v133 = llvm.call @a_add_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v133: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_sub_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.sub %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v71 = llvm.icmp "sge" %v69, %v70 : i64
  %v72 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v73 = llvm.icmp "slt" %v69, %v72 : i64
  %v74 = llvm.and %v71, %v73 : i1
  llvm.cond_br %v74, ^L5, ^L3
^L5:
  %v75 = llvm.mlir.constant(1 : i64) : i64
  %v76 = llvm.shl %v69, %v75 : i64
  %v77 = llvm.mlir.constant(1 : i64) : i64
  %v78 = llvm.or %v76, %v77 : i64
  %v79 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v80 = llvm.and %v78, %v79 : i64
  %v81 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v82 = llvm.or %v80, %v81 : i64
  llvm.br ^L4(%v82: i64)
^L2:
  %v83 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v84 = llvm.and %a0, %v83 : i64
  %v85 = llvm.icmp "eq" %v84, %v83 : i64
  %v86 = llvm.mlir.constant(1 : i64) : i64
  %v87 = llvm.and %a0, %v86 : i64
  %v88 = llvm.mlir.constant(0 : i64) : i64
  %v89 = llvm.icmp "ne" %v87, %v88 : i64
  %v90 = llvm.and %v85, %v89 : i1
  %v91 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v92 = llvm.and %a0, %v91 : i64
  %v93 = llvm.mlir.constant(1 : i64) : i64
  %v94 = llvm.lshr %v92, %v93 : i64
  %v95 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v96 = llvm.and %v94, %v95 : i64
  %v97 = llvm.mlir.constant(0 : i64) : i64
  %v98 = llvm.icmp "ne" %v96, %v97 : i64
  %v99 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v100 = llvm.or %v94, %v99 : i64
  %v101 = llvm.select %v98, %v100, %v94 : i1, i64
  %v102 = llvm.sitofp %v101 : i64 to f64
  %v103 = llvm.bitcast %a0 : i64 to f64
  %v104 = llvm.select %v90, %v102, %v103 : i1, f64
  %v105 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v106 = llvm.and %a1, %v105 : i64
  %v107 = llvm.icmp "eq" %v106, %v105 : i64
  %v108 = llvm.mlir.constant(1 : i64) : i64
  %v109 = llvm.and %a1, %v108 : i64
  %v110 = llvm.mlir.constant(0 : i64) : i64
  %v111 = llvm.icmp "ne" %v109, %v110 : i64
  %v112 = llvm.and %v107, %v111 : i1
  %v113 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v114 = llvm.and %a1, %v113 : i64
  %v115 = llvm.mlir.constant(1 : i64) : i64
  %v116 = llvm.lshr %v114, %v115 : i64
  %v117 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v118 = llvm.and %v116, %v117 : i64
  %v119 = llvm.mlir.constant(0 : i64) : i64
  %v120 = llvm.icmp "ne" %v118, %v119 : i64
  %v121 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v122 = llvm.or %v116, %v121 : i64
  %v123 = llvm.select %v120, %v122, %v116 : i1, i64
  %v124 = llvm.sitofp %v123 : i64 to f64
  %v125 = llvm.bitcast %a1 : i64 to f64
  %v126 = llvm.select %v112, %v124, %v125 : i1, f64
  %v127 = llvm.fsub %v104, %v126 : f64
  %v128 = llvm.fcmp "uno" %v127, %v127 : f64
  %v129 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v130 = llvm.bitcast %v129 : i64 to f64
  %v131 = llvm.select %v128, %v130, %v127 : i1, f64
  %v132 = llvm.bitcast %v131 : f64 to i64
  llvm.br ^L4(%v132: i64)
^L3:
  %v133 = llvm.call @a_sub_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v133: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_mul_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.mlir.constant(0 : i64) : i64
  %v70 = llvm.icmp "eq" %v57, %v69 : i64
  %v71 = llvm.mlir.constant(0 : i64) : i64
  %v72 = llvm.icmp "eq" %v68, %v71 : i64
  %v73 = llvm.or %v70, %v72 : i1
  llvm.cond_br %v73, ^L5, ^L6
^L5:
  %v74 = llvm.mlir.constant(0 : i64) : i64
  %v75 = llvm.mlir.constant(1 : i64) : i64
  %v76 = llvm.shl %v74, %v75 : i64
  %v77 = llvm.mlir.constant(1 : i64) : i64
  %v78 = llvm.or %v76, %v77 : i64
  %v79 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v80 = llvm.and %v78, %v79 : i64
  %v81 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v82 = llvm.or %v80, %v81 : i64
  llvm.br ^L4(%v82: i64)
^L6:
  %v83 = llvm.mlir.constant(0 : i64) : i64
  %v84 = llvm.icmp "slt" %v57, %v83 : i64
  %v85 = llvm.mlir.constant(0 : i64) : i64
  %v86 = llvm.sub %v85, %v57 : i64
  %v87 = llvm.select %v84, %v86, %v57 : i1, i64
  %v88 = llvm.mlir.constant(0 : i64) : i64
  %v89 = llvm.icmp "slt" %v68, %v88 : i64
  %v90 = llvm.mlir.constant(0 : i64) : i64
  %v91 = llvm.sub %v90, %v68 : i64
  %v92 = llvm.select %v89, %v91, %v68 : i1, i64
  %v93 = llvm.mlir.constant(0 : i64) : i64
  %v94 = llvm.icmp "eq" %v92, %v93 : i64
  %v95 = llvm.mlir.constant(1 : i64) : i64
  %v96 = llvm.select %v94, %v95, %v92 : i1, i64
  %v97 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v98 = llvm.sdiv %v97, %v96 : i64
  %v99 = llvm.icmp "sle" %v87, %v98 : i64
  llvm.cond_br %v99, ^L7, ^L3
^L7:
  %v100 = llvm.mul %v57, %v68 : i64
  %v101 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v102 = llvm.icmp "sge" %v100, %v101 : i64
  %v103 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v104 = llvm.icmp "slt" %v100, %v103 : i64
  %v105 = llvm.and %v102, %v104 : i1
  llvm.cond_br %v105, ^L8, ^L3
^L8:
  %v106 = llvm.mlir.constant(1 : i64) : i64
  %v107 = llvm.shl %v100, %v106 : i64
  %v108 = llvm.mlir.constant(1 : i64) : i64
  %v109 = llvm.or %v107, %v108 : i64
  %v110 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v111 = llvm.and %v109, %v110 : i64
  %v112 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v113 = llvm.or %v111, %v112 : i64
  llvm.br ^L4(%v113: i64)
^L2:
  %v114 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v115 = llvm.and %a0, %v114 : i64
  %v116 = llvm.icmp "eq" %v115, %v114 : i64
  %v117 = llvm.mlir.constant(1 : i64) : i64
  %v118 = llvm.and %a0, %v117 : i64
  %v119 = llvm.mlir.constant(0 : i64) : i64
  %v120 = llvm.icmp "ne" %v118, %v119 : i64
  %v121 = llvm.and %v116, %v120 : i1
  %v122 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v123 = llvm.and %a0, %v122 : i64
  %v124 = llvm.mlir.constant(1 : i64) : i64
  %v125 = llvm.lshr %v123, %v124 : i64
  %v126 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v127 = llvm.and %v125, %v126 : i64
  %v128 = llvm.mlir.constant(0 : i64) : i64
  %v129 = llvm.icmp "ne" %v127, %v128 : i64
  %v130 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v131 = llvm.or %v125, %v130 : i64
  %v132 = llvm.select %v129, %v131, %v125 : i1, i64
  %v133 = llvm.sitofp %v132 : i64 to f64
  %v134 = llvm.bitcast %a0 : i64 to f64
  %v135 = llvm.select %v121, %v133, %v134 : i1, f64
  %v136 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v137 = llvm.and %a1, %v136 : i64
  %v138 = llvm.icmp "eq" %v137, %v136 : i64
  %v139 = llvm.mlir.constant(1 : i64) : i64
  %v140 = llvm.and %a1, %v139 : i64
  %v141 = llvm.mlir.constant(0 : i64) : i64
  %v142 = llvm.icmp "ne" %v140, %v141 : i64
  %v143 = llvm.and %v138, %v142 : i1
  %v144 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v145 = llvm.and %a1, %v144 : i64
  %v146 = llvm.mlir.constant(1 : i64) : i64
  %v147 = llvm.lshr %v145, %v146 : i64
  %v148 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v149 = llvm.and %v147, %v148 : i64
  %v150 = llvm.mlir.constant(0 : i64) : i64
  %v151 = llvm.icmp "ne" %v149, %v150 : i64
  %v152 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v153 = llvm.or %v147, %v152 : i64
  %v154 = llvm.select %v151, %v153, %v147 : i1, i64
  %v155 = llvm.sitofp %v154 : i64 to f64
  %v156 = llvm.bitcast %a1 : i64 to f64
  %v157 = llvm.select %v143, %v155, %v156 : i1, f64
  %v158 = llvm.fmul %v135, %v157 : f64
  %v159 = llvm.fcmp "uno" %v158, %v158 : f64
  %v160 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v161 = llvm.bitcast %v160 : i64 to f64
  %v162 = llvm.select %v159, %v161, %v158 : i1, f64
  %v163 = llvm.bitcast %v162 : f64 to i64
  llvm.br ^L4(%v163: i64)
^L3:
  %v164 = llvm.call @a_mul_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v164: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_div_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a0, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(0 : i1) : i1
  %v12 = llvm.icmp "eq" %v10, %v11 : i1
  %v13 = llvm.or %v7, %v12 : i1
  %v14 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v15 = llvm.and %a1, %v14 : i64
  %v16 = llvm.icmp "eq" %v15, %v14 : i64
  %v17 = llvm.mlir.constant(1 : i64) : i64
  %v18 = llvm.and %a1, %v17 : i64
  %v19 = llvm.mlir.constant(0 : i64) : i64
  %v20 = llvm.icmp "ne" %v18, %v19 : i64
  %v21 = llvm.and %v16, %v20 : i1
  %v22 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v23 = llvm.and %a1, %v22 : i64
  %v24 = llvm.icmp "eq" %v23, %v22 : i64
  %v25 = llvm.mlir.constant(0 : i1) : i1
  %v26 = llvm.icmp "eq" %v24, %v25 : i1
  %v27 = llvm.or %v21, %v26 : i1
  %v28 = llvm.and %v13, %v27 : i1
  llvm.cond_br %v28, ^L0, ^L1
^L0:
  %v30 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v31 = llvm.and %a0, %v30 : i64
  %v32 = llvm.icmp "eq" %v31, %v30 : i64
  %v33 = llvm.mlir.constant(1 : i64) : i64
  %v34 = llvm.and %a0, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.and %v32, %v36 : i1
  %v38 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v39 = llvm.and %a0, %v38 : i64
  %v40 = llvm.mlir.constant(1 : i64) : i64
  %v41 = llvm.lshr %v39, %v40 : i64
  %v42 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v43 = llvm.and %v41, %v42 : i64
  %v44 = llvm.mlir.constant(0 : i64) : i64
  %v45 = llvm.icmp "ne" %v43, %v44 : i64
  %v46 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v47 = llvm.or %v41, %v46 : i64
  %v48 = llvm.select %v45, %v47, %v41 : i1, i64
  %v49 = llvm.sitofp %v48 : i64 to f64
  %v50 = llvm.bitcast %a0 : i64 to f64
  %v51 = llvm.select %v37, %v49, %v50 : i1, f64
  %v52 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v53 = llvm.and %a1, %v52 : i64
  %v54 = llvm.icmp "eq" %v53, %v52 : i64
  %v55 = llvm.mlir.constant(1 : i64) : i64
  %v56 = llvm.and %a1, %v55 : i64
  %v57 = llvm.mlir.constant(0 : i64) : i64
  %v58 = llvm.icmp "ne" %v56, %v57 : i64
  %v59 = llvm.and %v54, %v58 : i1
  %v60 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v61 = llvm.and %a1, %v60 : i64
  %v62 = llvm.mlir.constant(1 : i64) : i64
  %v63 = llvm.lshr %v61, %v62 : i64
  %v64 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v65 = llvm.and %v63, %v64 : i64
  %v66 = llvm.mlir.constant(0 : i64) : i64
  %v67 = llvm.icmp "ne" %v65, %v66 : i64
  %v68 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v69 = llvm.or %v63, %v68 : i64
  %v70 = llvm.select %v67, %v69, %v63 : i1, i64
  %v71 = llvm.sitofp %v70 : i64 to f64
  %v72 = llvm.bitcast %a1 : i64 to f64
  %v73 = llvm.select %v59, %v71, %v72 : i1, f64
  %v74 = llvm.mlir.constant(0 : i64) : i64
  %v75 = llvm.bitcast %v74 : i64 to f64
  %v76 = llvm.fcmp "oeq" %v73, %v75 : f64
  llvm.cond_br %v76, ^L1, ^L3
^L3:
  %v77 = llvm.fdiv %v51, %v73 : f64
  %v78 = llvm.fcmp "uno" %v77, %v77 : f64
  %v79 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v80 = llvm.bitcast %v79 : i64 to f64
  %v81 = llvm.select %v78, %v80, %v77 : i1, f64
  %v82 = llvm.bitcast %v81 : f64 to i64
  llvm.br ^L2(%v82: i64)
^L1:
  %v83 = llvm.call @a_div_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v83: i64)
^L2(%v29: i64):
  llvm.return %v29 : i64
}

llvm.func @a_floordiv_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.mlir.constant(0 : i64) : i64
  %v70 = llvm.icmp "eq" %v68, %v69 : i64
  llvm.cond_br %v70, ^L3, ^L5
^L5:
  %v71 = llvm.sdiv %v57, %v68 : i64
  %v72 = llvm.srem %v57, %v68 : i64
  %v73 = llvm.mlir.constant(0 : i64) : i64
  %v74 = llvm.icmp "ne" %v72, %v73 : i64
  %v75 = llvm.mlir.constant(0 : i64) : i64
  %v76 = llvm.icmp "slt" %v72, %v75 : i64
  %v77 = llvm.mlir.constant(0 : i64) : i64
  %v78 = llvm.icmp "slt" %v68, %v77 : i64
  %v79 = llvm.xor %v76, %v78 : i1
  %v80 = llvm.and %v74, %v79 : i1
  %v81 = llvm.mlir.constant(1 : i64) : i64
  %v82 = llvm.sub %v71, %v81 : i64
  %v83 = llvm.select %v80, %v82, %v71 : i1, i64
  %v84 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v85 = llvm.icmp "sge" %v83, %v84 : i64
  %v86 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v87 = llvm.icmp "slt" %v83, %v86 : i64
  %v88 = llvm.and %v85, %v87 : i1
  llvm.cond_br %v88, ^L6, ^L3
^L6:
  %v89 = llvm.mlir.constant(1 : i64) : i64
  %v90 = llvm.shl %v83, %v89 : i64
  %v91 = llvm.mlir.constant(1 : i64) : i64
  %v92 = llvm.or %v90, %v91 : i64
  %v93 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v94 = llvm.and %v92, %v93 : i64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.or %v94, %v95 : i64
  llvm.br ^L4(%v96: i64)
^L2:
  %v97 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v98 = llvm.and %a0, %v97 : i64
  %v99 = llvm.icmp "eq" %v98, %v97 : i64
  %v100 = llvm.mlir.constant(1 : i64) : i64
  %v101 = llvm.and %a0, %v100 : i64
  %v102 = llvm.mlir.constant(0 : i64) : i64
  %v103 = llvm.icmp "ne" %v101, %v102 : i64
  %v104 = llvm.and %v99, %v103 : i1
  %v105 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v106 = llvm.and %a0, %v105 : i64
  %v107 = llvm.mlir.constant(1 : i64) : i64
  %v108 = llvm.lshr %v106, %v107 : i64
  %v109 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v110 = llvm.and %v108, %v109 : i64
  %v111 = llvm.mlir.constant(0 : i64) : i64
  %v112 = llvm.icmp "ne" %v110, %v111 : i64
  %v113 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v114 = llvm.or %v108, %v113 : i64
  %v115 = llvm.select %v112, %v114, %v108 : i1, i64
  %v116 = llvm.sitofp %v115 : i64 to f64
  %v117 = llvm.bitcast %a0 : i64 to f64
  %v118 = llvm.select %v104, %v116, %v117 : i1, f64
  %v119 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v120 = llvm.and %a1, %v119 : i64
  %v121 = llvm.icmp "eq" %v120, %v119 : i64
  %v122 = llvm.mlir.constant(1 : i64) : i64
  %v123 = llvm.and %a1, %v122 : i64
  %v124 = llvm.mlir.constant(0 : i64) : i64
  %v125 = llvm.icmp "ne" %v123, %v124 : i64
  %v126 = llvm.and %v121, %v125 : i1
  %v127 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v128 = llvm.and %a1, %v127 : i64
  %v129 = llvm.mlir.constant(1 : i64) : i64
  %v130 = llvm.lshr %v128, %v129 : i64
  %v131 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v132 = llvm.and %v130, %v131 : i64
  %v133 = llvm.mlir.constant(0 : i64) : i64
  %v134 = llvm.icmp "ne" %v132, %v133 : i64
  %v135 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v136 = llvm.or %v130, %v135 : i64
  %v137 = llvm.select %v134, %v136, %v130 : i1, i64
  %v138 = llvm.sitofp %v137 : i64 to f64
  %v139 = llvm.bitcast %a1 : i64 to f64
  %v140 = llvm.select %v126, %v138, %v139 : i1, f64
  %v141 = llvm.mlir.constant(0 : i64) : i64
  %v142 = llvm.bitcast %v141 : i64 to f64
  %v143 = llvm.fcmp "oeq" %v140, %v142 : f64
  llvm.cond_br %v143, ^L3, ^L7
^L7:
  %v144 = llvm.fdiv %v118, %v140 : f64
  %v145 = llvm.call @floor(%v144) : (f64) -> f64
  %v146 = llvm.fcmp "uno" %v145, %v145 : f64
  %v147 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v148 = llvm.bitcast %v147 : i64 to f64
  %v149 = llvm.select %v146, %v148, %v145 : i1, f64
  %v150 = llvm.bitcast %v149 : f64 to i64
  llvm.br ^L4(%v150: i64)
^L3:
  %v151 = llvm.call @a_floordiv_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v151: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_mod_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.mlir.constant(0 : i64) : i64
  %v70 = llvm.icmp "eq" %v68, %v69 : i64
  llvm.cond_br %v70, ^L3, ^L5
^L5:
  %v71 = llvm.mlir.constant(-1 : i64) : i64
  %v72 = llvm.icmp "eq" %v68, %v71 : i64
  llvm.cond_br %v72, ^L7, ^L6
^L7:
  %v73 = llvm.mlir.constant(0 : i64) : i64
  %v74 = llvm.mlir.constant(1 : i64) : i64
  %v75 = llvm.shl %v73, %v74 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.or %v75, %v76 : i64
  %v78 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v79 = llvm.and %v77, %v78 : i64
  %v80 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v81 = llvm.or %v79, %v80 : i64
  llvm.br ^L4(%v81: i64)
^L6:
  %v82 = llvm.srem %v57, %v68 : i64
  %v83 = llvm.mlir.constant(0 : i64) : i64
  %v84 = llvm.icmp "ne" %v82, %v83 : i64
  %v85 = llvm.mlir.constant(0 : i64) : i64
  %v86 = llvm.icmp "slt" %v82, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "slt" %v68, %v87 : i64
  %v89 = llvm.xor %v86, %v88 : i1
  %v90 = llvm.and %v84, %v89 : i1
  %v91 = llvm.add %v82, %v68 : i64
  %v92 = llvm.select %v90, %v91, %v82 : i1, i64
  %v93 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v94 = llvm.icmp "sge" %v92, %v93 : i64
  %v95 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v96 = llvm.icmp "slt" %v92, %v95 : i64
  %v97 = llvm.and %v94, %v96 : i1
  llvm.cond_br %v97, ^L8, ^L3
^L8:
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.shl %v92, %v98 : i64
  %v100 = llvm.mlir.constant(1 : i64) : i64
  %v101 = llvm.or %v99, %v100 : i64
  %v102 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v103 = llvm.and %v101, %v102 : i64
  %v104 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v105 = llvm.or %v103, %v104 : i64
  llvm.br ^L4(%v105: i64)
^L2:
  %v106 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v107 = llvm.and %a0, %v106 : i64
  %v108 = llvm.icmp "eq" %v107, %v106 : i64
  %v109 = llvm.mlir.constant(1 : i64) : i64
  %v110 = llvm.and %a0, %v109 : i64
  %v111 = llvm.mlir.constant(0 : i64) : i64
  %v112 = llvm.icmp "ne" %v110, %v111 : i64
  %v113 = llvm.and %v108, %v112 : i1
  %v114 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v115 = llvm.and %a0, %v114 : i64
  %v116 = llvm.mlir.constant(1 : i64) : i64
  %v117 = llvm.lshr %v115, %v116 : i64
  %v118 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v119 = llvm.and %v117, %v118 : i64
  %v120 = llvm.mlir.constant(0 : i64) : i64
  %v121 = llvm.icmp "ne" %v119, %v120 : i64
  %v122 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v123 = llvm.or %v117, %v122 : i64
  %v124 = llvm.select %v121, %v123, %v117 : i1, i64
  %v125 = llvm.sitofp %v124 : i64 to f64
  %v126 = llvm.bitcast %a0 : i64 to f64
  %v127 = llvm.select %v113, %v125, %v126 : i1, f64
  %v128 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v129 = llvm.and %a1, %v128 : i64
  %v130 = llvm.icmp "eq" %v129, %v128 : i64
  %v131 = llvm.mlir.constant(1 : i64) : i64
  %v132 = llvm.and %a1, %v131 : i64
  %v133 = llvm.mlir.constant(0 : i64) : i64
  %v134 = llvm.icmp "ne" %v132, %v133 : i64
  %v135 = llvm.and %v130, %v134 : i1
  %v136 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v137 = llvm.and %a1, %v136 : i64
  %v138 = llvm.mlir.constant(1 : i64) : i64
  %v139 = llvm.lshr %v137, %v138 : i64
  %v140 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v141 = llvm.and %v139, %v140 : i64
  %v142 = llvm.mlir.constant(0 : i64) : i64
  %v143 = llvm.icmp "ne" %v141, %v142 : i64
  %v144 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v145 = llvm.or %v139, %v144 : i64
  %v146 = llvm.select %v143, %v145, %v139 : i1, i64
  %v147 = llvm.sitofp %v146 : i64 to f64
  %v148 = llvm.bitcast %a1 : i64 to f64
  %v149 = llvm.select %v135, %v147, %v148 : i1, f64
  %v150 = llvm.mlir.constant(0 : i64) : i64
  %v151 = llvm.bitcast %v150 : i64 to f64
  %v152 = llvm.fcmp "oeq" %v149, %v151 : f64
  llvm.cond_br %v152, ^L3, ^L9
^L9:
  %v153 = llvm.call @fmod(%v127, %v149) : (f64, f64) -> f64
  %v154 = llvm.mlir.constant(0 : i64) : i64
  %v155 = llvm.bitcast %v154 : i64 to f64
  %v156 = llvm.fcmp "une" %v153, %v155 : f64
  %v157 = llvm.mlir.constant(0 : i64) : i64
  %v158 = llvm.bitcast %v157 : i64 to f64
  %v159 = llvm.fcmp "ogt" %v153, %v158 : f64
  %v160 = llvm.mlir.constant(0 : i64) : i64
  %v161 = llvm.bitcast %v160 : i64 to f64
  %v162 = llvm.fcmp "ogt" %v149, %v161 : f64
  %v163 = llvm.xor %v159, %v162 : i1
  %v164 = llvm.and %v156, %v163 : i1
  %v165 = llvm.fadd %v153, %v149 : f64
  %v166 = llvm.select %v164, %v165, %v153 : i1, f64
  %v167 = llvm.fcmp "uno" %v166, %v166 : f64
  %v168 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v169 = llvm.bitcast %v168 : i64 to f64
  %v170 = llvm.select %v167, %v169, %v166 : i1, f64
  %v171 = llvm.bitcast %v170 : f64 to i64
  llvm.br ^L4(%v171: i64)
^L3:
  %v172 = llvm.call @a_mod_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v172: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_eq_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "eq" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "oeq" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_eq_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_ne_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "ne" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "une" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_ne_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_lt_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "slt" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "olt" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_lt_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_le_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "sle" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "ole" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_le_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_gt_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "sgt" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "ogt" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_gt_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_ge_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  %v17 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v18 = llvm.and %a0, %v17 : i64
  %v19 = llvm.icmp "eq" %v18, %v17 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.and %a0, %v20 : i64
  %v22 = llvm.mlir.constant(0 : i64) : i64
  %v23 = llvm.icmp "ne" %v21, %v22 : i64
  %v24 = llvm.and %v19, %v23 : i1
  %v25 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v26 = llvm.and %a0, %v25 : i64
  %v27 = llvm.icmp "eq" %v26, %v25 : i64
  %v28 = llvm.mlir.constant(0 : i1) : i1
  %v29 = llvm.icmp "eq" %v27, %v28 : i1
  %v30 = llvm.or %v24, %v29 : i1
  %v31 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v32 = llvm.and %a1, %v31 : i64
  %v33 = llvm.icmp "eq" %v32, %v31 : i64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.and %a1, %v34 : i64
  %v36 = llvm.mlir.constant(0 : i64) : i64
  %v37 = llvm.icmp "ne" %v35, %v36 : i64
  %v38 = llvm.and %v33, %v37 : i1
  %v39 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v40 = llvm.and %a1, %v39 : i64
  %v41 = llvm.icmp "eq" %v40, %v39 : i64
  %v42 = llvm.mlir.constant(0 : i1) : i1
  %v43 = llvm.icmp "eq" %v41, %v42 : i1
  %v44 = llvm.or %v38, %v43 : i1
  %v45 = llvm.and %v30, %v44 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L1:
  llvm.cond_br %v45, ^L2, ^L3
^L0:
  %v47 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v48 = llvm.and %a0, %v47 : i64
  %v49 = llvm.mlir.constant(1 : i64) : i64
  %v50 = llvm.lshr %v48, %v49 : i64
  %v51 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v52 = llvm.and %v50, %v51 : i64
  %v53 = llvm.mlir.constant(0 : i64) : i64
  %v54 = llvm.icmp "ne" %v52, %v53 : i64
  %v55 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v56 = llvm.or %v50, %v55 : i64
  %v57 = llvm.select %v54, %v56, %v50 : i1, i64
  %v58 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v59 = llvm.and %a1, %v58 : i64
  %v60 = llvm.mlir.constant(1 : i64) : i64
  %v61 = llvm.lshr %v59, %v60 : i64
  %v62 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v63 = llvm.and %v61, %v62 : i64
  %v64 = llvm.mlir.constant(0 : i64) : i64
  %v65 = llvm.icmp "ne" %v63, %v64 : i64
  %v66 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v67 = llvm.or %v61, %v66 : i64
  %v68 = llvm.select %v65, %v67, %v61 : i1, i64
  %v69 = llvm.icmp "sge" %v57, %v68 : i64
  %v70 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v71 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v72 = llvm.select %v69, %v70, %v71 : i1, i64
  llvm.br ^L4(%v72: i64)
^L2:
  %v73 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v74 = llvm.and %a0, %v73 : i64
  %v75 = llvm.icmp "eq" %v74, %v73 : i64
  %v76 = llvm.mlir.constant(1 : i64) : i64
  %v77 = llvm.and %a0, %v76 : i64
  %v78 = llvm.mlir.constant(0 : i64) : i64
  %v79 = llvm.icmp "ne" %v77, %v78 : i64
  %v80 = llvm.and %v75, %v79 : i1
  %v81 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v82 = llvm.and %a0, %v81 : i64
  %v83 = llvm.mlir.constant(1 : i64) : i64
  %v84 = llvm.lshr %v82, %v83 : i64
  %v85 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v86 = llvm.and %v84, %v85 : i64
  %v87 = llvm.mlir.constant(0 : i64) : i64
  %v88 = llvm.icmp "ne" %v86, %v87 : i64
  %v89 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v90 = llvm.or %v84, %v89 : i64
  %v91 = llvm.select %v88, %v90, %v84 : i1, i64
  %v92 = llvm.sitofp %v91 : i64 to f64
  %v93 = llvm.bitcast %a0 : i64 to f64
  %v94 = llvm.select %v80, %v92, %v93 : i1, f64
  %v95 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v96 = llvm.and %a1, %v95 : i64
  %v97 = llvm.icmp "eq" %v96, %v95 : i64
  %v98 = llvm.mlir.constant(1 : i64) : i64
  %v99 = llvm.and %a1, %v98 : i64
  %v100 = llvm.mlir.constant(0 : i64) : i64
  %v101 = llvm.icmp "ne" %v99, %v100 : i64
  %v102 = llvm.and %v97, %v101 : i1
  %v103 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v104 = llvm.and %a1, %v103 : i64
  %v105 = llvm.mlir.constant(1 : i64) : i64
  %v106 = llvm.lshr %v104, %v105 : i64
  %v107 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v108 = llvm.and %v106, %v107 : i64
  %v109 = llvm.mlir.constant(0 : i64) : i64
  %v110 = llvm.icmp "ne" %v108, %v109 : i64
  %v111 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v112 = llvm.or %v106, %v111 : i64
  %v113 = llvm.select %v110, %v112, %v106 : i1, i64
  %v114 = llvm.sitofp %v113 : i64 to f64
  %v115 = llvm.bitcast %a1 : i64 to f64
  %v116 = llvm.select %v102, %v114, %v115 : i1, f64
  %v117 = llvm.fcmp "oge" %v94, %v116 : f64
  %v118 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v119 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v120 = llvm.select %v117, %v118, %v119 : i1, i64
  llvm.br ^L4(%v120: i64)
^L3:
  %v121 = llvm.call @a_ge_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L4(%v121: i64)
^L4(%v46: i64):
  llvm.return %v46 : i64
}

llvm.func @a_band_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L0:
  %v18 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v19 = llvm.and %a0, %v18 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.lshr %v19, %v20 : i64
  %v22 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v23 = llvm.and %v21, %v22 : i64
  %v24 = llvm.mlir.constant(0 : i64) : i64
  %v25 = llvm.icmp "ne" %v23, %v24 : i64
  %v26 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v27 = llvm.or %v21, %v26 : i64
  %v28 = llvm.select %v25, %v27, %v21 : i1, i64
  %v29 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v30 = llvm.and %a1, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.lshr %v30, %v31 : i64
  %v33 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v38 = llvm.or %v32, %v37 : i64
  %v39 = llvm.select %v36, %v38, %v32 : i1, i64
  %v40 = llvm.and %v28, %v39 : i64
  %v41 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v42 = llvm.icmp "sge" %v40, %v41 : i64
  %v43 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v44 = llvm.icmp "slt" %v40, %v43 : i64
  %v45 = llvm.and %v42, %v44 : i1
  llvm.cond_br %v45, ^L3, ^L1
^L3:
  %v46 = llvm.mlir.constant(1 : i64) : i64
  %v47 = llvm.shl %v40, %v46 : i64
  %v48 = llvm.mlir.constant(1 : i64) : i64
  %v49 = llvm.or %v47, %v48 : i64
  %v50 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v51 = llvm.and %v49, %v50 : i64
  %v52 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v53 = llvm.or %v51, %v52 : i64
  llvm.br ^L2(%v53: i64)
^L1:
  %v54 = llvm.call @a_band_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v54: i64)
^L2(%v17: i64):
  llvm.return %v17 : i64
}

llvm.func @a_bor_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L0:
  %v18 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v19 = llvm.and %a0, %v18 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.lshr %v19, %v20 : i64
  %v22 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v23 = llvm.and %v21, %v22 : i64
  %v24 = llvm.mlir.constant(0 : i64) : i64
  %v25 = llvm.icmp "ne" %v23, %v24 : i64
  %v26 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v27 = llvm.or %v21, %v26 : i64
  %v28 = llvm.select %v25, %v27, %v21 : i1, i64
  %v29 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v30 = llvm.and %a1, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.lshr %v30, %v31 : i64
  %v33 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v38 = llvm.or %v32, %v37 : i64
  %v39 = llvm.select %v36, %v38, %v32 : i1, i64
  %v40 = llvm.or %v28, %v39 : i64
  %v41 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v42 = llvm.icmp "sge" %v40, %v41 : i64
  %v43 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v44 = llvm.icmp "slt" %v40, %v43 : i64
  %v45 = llvm.and %v42, %v44 : i1
  llvm.cond_br %v45, ^L3, ^L1
^L3:
  %v46 = llvm.mlir.constant(1 : i64) : i64
  %v47 = llvm.shl %v40, %v46 : i64
  %v48 = llvm.mlir.constant(1 : i64) : i64
  %v49 = llvm.or %v47, %v48 : i64
  %v50 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v51 = llvm.and %v49, %v50 : i64
  %v52 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v53 = llvm.or %v51, %v52 : i64
  llvm.br ^L2(%v53: i64)
^L1:
  %v54 = llvm.call @a_bor_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v54: i64)
^L2(%v17: i64):
  llvm.return %v17 : i64
}

llvm.func @a_bxor_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L0:
  %v18 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v19 = llvm.and %a0, %v18 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.lshr %v19, %v20 : i64
  %v22 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v23 = llvm.and %v21, %v22 : i64
  %v24 = llvm.mlir.constant(0 : i64) : i64
  %v25 = llvm.icmp "ne" %v23, %v24 : i64
  %v26 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v27 = llvm.or %v21, %v26 : i64
  %v28 = llvm.select %v25, %v27, %v21 : i1, i64
  %v29 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v30 = llvm.and %a1, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.lshr %v30, %v31 : i64
  %v33 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v38 = llvm.or %v32, %v37 : i64
  %v39 = llvm.select %v36, %v38, %v32 : i1, i64
  %v40 = llvm.xor %v28, %v39 : i64
  %v41 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v42 = llvm.icmp "sge" %v40, %v41 : i64
  %v43 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v44 = llvm.icmp "slt" %v40, %v43 : i64
  %v45 = llvm.and %v42, %v44 : i1
  llvm.cond_br %v45, ^L3, ^L1
^L3:
  %v46 = llvm.mlir.constant(1 : i64) : i64
  %v47 = llvm.shl %v40, %v46 : i64
  %v48 = llvm.mlir.constant(1 : i64) : i64
  %v49 = llvm.or %v47, %v48 : i64
  %v50 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v51 = llvm.and %v49, %v50 : i64
  %v52 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v53 = llvm.or %v51, %v52 : i64
  llvm.br ^L2(%v53: i64)
^L1:
  %v54 = llvm.call @a_bxor_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v54: i64)
^L2(%v17: i64):
  llvm.return %v17 : i64
}

llvm.func @a_shl_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L0:
  %v18 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v19 = llvm.and %a0, %v18 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.lshr %v19, %v20 : i64
  %v22 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v23 = llvm.and %v21, %v22 : i64
  %v24 = llvm.mlir.constant(0 : i64) : i64
  %v25 = llvm.icmp "ne" %v23, %v24 : i64
  %v26 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v27 = llvm.or %v21, %v26 : i64
  %v28 = llvm.select %v25, %v27, %v21 : i1, i64
  %v29 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v30 = llvm.and %a1, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.lshr %v30, %v31 : i64
  %v33 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v38 = llvm.or %v32, %v37 : i64
  %v39 = llvm.select %v36, %v38, %v32 : i1, i64
  %v40 = llvm.mlir.constant(0 : i64) : i64
  %v41 = llvm.icmp "sge" %v39, %v40 : i64
  %v42 = llvm.mlir.constant(63 : i64) : i64
  %v43 = llvm.icmp "sle" %v39, %v42 : i64
  %v44 = llvm.and %v41, %v43 : i1
  llvm.cond_br %v44, ^L3, ^L1
^L3:
  %v45 = llvm.shl %v28, %v39 : i64
  %v46 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v47 = llvm.icmp "sge" %v45, %v46 : i64
  %v48 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v49 = llvm.icmp "slt" %v45, %v48 : i64
  %v50 = llvm.and %v47, %v49 : i1
  llvm.cond_br %v50, ^L4, ^L1
^L4:
  %v51 = llvm.mlir.constant(1 : i64) : i64
  %v52 = llvm.shl %v45, %v51 : i64
  %v53 = llvm.mlir.constant(1 : i64) : i64
  %v54 = llvm.or %v52, %v53 : i64
  %v55 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v56 = llvm.and %v54, %v55 : i64
  %v57 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v58 = llvm.or %v56, %v57 : i64
  llvm.br ^L2(%v58: i64)
^L1:
  %v59 = llvm.call @a_shl_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v59: i64)
^L2(%v17: i64):
  llvm.return %v17 : i64
}

llvm.func @a_shr_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a1, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.and %a1, %v11 : i64
  %v13 = llvm.mlir.constant(0 : i64) : i64
  %v14 = llvm.icmp "ne" %v12, %v13 : i64
  %v15 = llvm.and %v10, %v14 : i1
  %v16 = llvm.and %v7, %v15 : i1
  llvm.cond_br %v16, ^L0, ^L1
^L0:
  %v18 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v19 = llvm.and %a0, %v18 : i64
  %v20 = llvm.mlir.constant(1 : i64) : i64
  %v21 = llvm.lshr %v19, %v20 : i64
  %v22 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v23 = llvm.and %v21, %v22 : i64
  %v24 = llvm.mlir.constant(0 : i64) : i64
  %v25 = llvm.icmp "ne" %v23, %v24 : i64
  %v26 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v27 = llvm.or %v21, %v26 : i64
  %v28 = llvm.select %v25, %v27, %v21 : i1, i64
  %v29 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v30 = llvm.and %a1, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.lshr %v30, %v31 : i64
  %v33 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.icmp "ne" %v34, %v35 : i64
  %v37 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v38 = llvm.or %v32, %v37 : i64
  %v39 = llvm.select %v36, %v38, %v32 : i1, i64
  %v40 = llvm.mlir.constant(0 : i64) : i64
  %v41 = llvm.icmp "sge" %v39, %v40 : i64
  %v42 = llvm.mlir.constant(63 : i64) : i64
  %v43 = llvm.icmp "sle" %v39, %v42 : i64
  %v44 = llvm.and %v41, %v43 : i1
  llvm.cond_br %v44, ^L3, ^L1
^L3:
  %v45 = llvm.ashr %v28, %v39 : i64
  %v46 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v47 = llvm.icmp "sge" %v45, %v46 : i64
  %v48 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v49 = llvm.icmp "slt" %v45, %v48 : i64
  %v50 = llvm.and %v47, %v49 : i1
  llvm.cond_br %v50, ^L4, ^L1
^L4:
  %v51 = llvm.mlir.constant(1 : i64) : i64
  %v52 = llvm.shl %v45, %v51 : i64
  %v53 = llvm.mlir.constant(1 : i64) : i64
  %v54 = llvm.or %v52, %v53 : i64
  %v55 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v56 = llvm.and %v54, %v55 : i64
  %v57 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v58 = llvm.or %v56, %v57 : i64
  llvm.br ^L2(%v58: i64)
^L1:
  %v59 = llvm.call @a_shr_slow(%a0, %a1) : (i64, i64) -> i64
  llvm.br ^L2(%v59: i64)
^L2(%v17: i64):
  llvm.return %v17 : i64
}

llvm.func @a_neg_fast(%a0: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a0, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(0 : i1) : i1
  %v12 = llvm.icmp "eq" %v10, %v11 : i1
  llvm.cond_br %v7, ^L0, ^L1
^L0:
  %v14 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v15 = llvm.and %a0, %v14 : i64
  %v16 = llvm.mlir.constant(1 : i64) : i64
  %v17 = llvm.lshr %v15, %v16 : i64
  %v18 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v19 = llvm.and %v17, %v18 : i64
  %v20 = llvm.mlir.constant(0 : i64) : i64
  %v21 = llvm.icmp "ne" %v19, %v20 : i64
  %v22 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v23 = llvm.or %v17, %v22 : i64
  %v24 = llvm.select %v21, %v23, %v17 : i1, i64
  %v25 = llvm.mlir.constant(0 : i64) : i64
  %v26 = llvm.sub %v25, %v24 : i64
  %v27 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v28 = llvm.icmp "ne" %v24, %v27 : i64
  llvm.cond_br %v28, ^L5, ^L3
^L5:
  %v29 = llvm.mlir.constant(1 : i64) : i64
  %v30 = llvm.shl %v26, %v29 : i64
  %v31 = llvm.mlir.constant(1 : i64) : i64
  %v32 = llvm.or %v30, %v31 : i64
  %v33 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v34 = llvm.and %v32, %v33 : i64
  %v35 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v36 = llvm.or %v34, %v35 : i64
  llvm.br ^L4(%v36: i64)
^L1:
  llvm.cond_br %v12, ^L2, ^L3
^L2:
  %v37 = llvm.bitcast %a0 : i64 to f64
  %v38 = llvm.bitcast %v37 : f64 to i64
  %v39 = llvm.mlir.constant(-9223372036854775808 : i64) : i64
  %v40 = llvm.xor %v38, %v39 : i64
  %v41 = llvm.bitcast %v40 : i64 to f64
  %v42 = llvm.fcmp "uno" %v41, %v41 : f64
  %v43 = llvm.mlir.constant(9221120237041090560 : i64) : i64
  %v44 = llvm.bitcast %v43 : i64 to f64
  %v45 = llvm.select %v42, %v44, %v41 : i1, f64
  %v46 = llvm.bitcast %v45 : f64 to i64
  llvm.br ^L4(%v46: i64)
^L3:
  %v47 = llvm.call @a_neg_slow(%a0) : (i64) -> i64
  llvm.br ^L4(%v47: i64)
^L4(%v13: i64):
  llvm.return %v13 : i64
}

llvm.func @a_bnot_fast(%a0: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  llvm.cond_br %v7, ^L0, ^L1
^L0:
  %v9 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v10 = llvm.and %a0, %v9 : i64
  %v11 = llvm.mlir.constant(1 : i64) : i64
  %v12 = llvm.lshr %v10, %v11 : i64
  %v13 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v14 = llvm.and %v12, %v13 : i64
  %v15 = llvm.mlir.constant(0 : i64) : i64
  %v16 = llvm.icmp "ne" %v14, %v15 : i64
  %v17 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v18 = llvm.or %v12, %v17 : i64
  %v19 = llvm.select %v16, %v18, %v12 : i1, i64
  %v20 = llvm.mlir.constant(-1 : i64) : i64
  %v21 = llvm.xor %v19, %v20 : i64
  %v22 = llvm.mlir.constant(-70368744177664 : i64) : i64
  %v23 = llvm.icmp "sge" %v21, %v22 : i64
  %v24 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v25 = llvm.icmp "slt" %v21, %v24 : i64
  %v26 = llvm.and %v23, %v25 : i1
  llvm.cond_br %v26, ^L3, ^L1
^L3:
  %v27 = llvm.mlir.constant(1 : i64) : i64
  %v28 = llvm.shl %v21, %v27 : i64
  %v29 = llvm.mlir.constant(1 : i64) : i64
  %v30 = llvm.or %v28, %v29 : i64
  %v31 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v32 = llvm.and %v30, %v31 : i64
  %v33 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v34 = llvm.or %v32, %v33 : i64
  llvm.br ^L2(%v34: i64)
^L1:
  %v35 = llvm.call @a_bnot_slow(%a0) : (i64) -> i64
  llvm.br ^L2(%v35: i64)
^L2(%v8: i64):
  llvm.return %v8 : i64
}

llvm.func @a_not_fast(%a0: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a0, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(0 : i1) : i1
  %v12 = llvm.icmp "eq" %v10, %v11 : i1
  llvm.cond_br %v7, ^L0, ^L1
^L0:
  %v14 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v15 = llvm.and %a0, %v14 : i64
  %v16 = llvm.mlir.constant(1 : i64) : i64
  %v17 = llvm.lshr %v15, %v16 : i64
  %v18 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v19 = llvm.and %v17, %v18 : i64
  %v20 = llvm.mlir.constant(0 : i64) : i64
  %v21 = llvm.icmp "ne" %v19, %v20 : i64
  %v22 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v23 = llvm.or %v17, %v22 : i64
  %v24 = llvm.select %v21, %v23, %v17 : i1, i64
  %v25 = llvm.mlir.constant(0 : i64) : i64
  %v26 = llvm.icmp "eq" %v24, %v25 : i64
  %v27 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v28 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v29 = llvm.select %v26, %v27, %v28 : i1, i64
  llvm.br ^L8(%v29: i64)
^L1:
  llvm.cond_br %v12, ^L2, ^L5
^L2:
  %v30 = llvm.bitcast %a0 : i64 to f64
  %v31 = llvm.mlir.constant(0 : i64) : i64
  %v32 = llvm.bitcast %v31 : i64 to f64
  %v33 = llvm.fcmp "oeq" %v30, %v32 : f64
  %v34 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v35 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v36 = llvm.select %v33, %v34, %v35 : i1, i64
  llvm.br ^L8(%v36: i64)
^L5:
  %v37 = llvm.mlir.constant(-281474976710654 : i64) : i64
  %v38 = llvm.icmp "eq" %a0, %v37 : i64
  %v39 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v40 = llvm.icmp "eq" %a0, %v39 : i64
  %v41 = llvm.or %v38, %v40 : i1
  llvm.cond_br %v41, ^L3, ^L4
^L3:
  %v42 = llvm.mlir.constant(-281474976710650 : i64) : i64
  llvm.br ^L8(%v42: i64)
^L4:
  %v43 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v44 = llvm.icmp "eq" %a0, %v43 : i64
  llvm.cond_br %v44, ^L6, ^L7
^L6:
  %v45 = llvm.mlir.constant(-281474976710652 : i64) : i64
  llvm.br ^L8(%v45: i64)
^L7:
  %v46 = llvm.call @a_not(%a0) : (i64) -> i64
  llvm.br ^L8(%v46: i64)
^L8(%v13: i64):
  llvm.return %v13 : i64
}

llvm.func @a_truthy_raw_fast(%a0: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v1 = llvm.and %a0, %v0 : i64
  %v2 = llvm.icmp "eq" %v1, %v0 : i64
  %v3 = llvm.mlir.constant(1 : i64) : i64
  %v4 = llvm.and %a0, %v3 : i64
  %v5 = llvm.mlir.constant(0 : i64) : i64
  %v6 = llvm.icmp "ne" %v4, %v5 : i64
  %v7 = llvm.and %v2, %v6 : i1
  %v8 = llvm.mlir.constant(-281474976710656 : i64) : i64
  %v9 = llvm.and %a0, %v8 : i64
  %v10 = llvm.icmp "eq" %v9, %v8 : i64
  %v11 = llvm.mlir.constant(0 : i1) : i1
  %v12 = llvm.icmp "eq" %v10, %v11 : i1
  llvm.cond_br %v7, ^L0, ^L1
^L0:
  %v14 = llvm.mlir.constant(281474976710655 : i64) : i64
  %v15 = llvm.and %a0, %v14 : i64
  %v16 = llvm.mlir.constant(1 : i64) : i64
  %v17 = llvm.lshr %v15, %v16 : i64
  %v18 = llvm.mlir.constant(70368744177664 : i64) : i64
  %v19 = llvm.and %v17, %v18 : i64
  %v20 = llvm.mlir.constant(0 : i64) : i64
  %v21 = llvm.icmp "ne" %v19, %v20 : i64
  %v22 = llvm.mlir.constant(-140737488355328 : i64) : i64
  %v23 = llvm.or %v17, %v22 : i64
  %v24 = llvm.select %v21, %v23, %v17 : i1, i64
  %v25 = llvm.mlir.constant(0 : i64) : i64
  %v26 = llvm.icmp "ne" %v24, %v25 : i64
  %v27 = llvm.mlir.constant(1 : i64) : i64
  %v28 = llvm.mlir.constant(0 : i64) : i64
  %v29 = llvm.select %v26, %v27, %v28 : i1, i64
  llvm.br ^L5(%v29: i64)
^L1:
  llvm.cond_br %v12, ^L2, ^L3
^L2:
  %v30 = llvm.bitcast %a0 : i64 to f64
  %v31 = llvm.mlir.constant(0 : i64) : i64
  %v32 = llvm.bitcast %v31 : i64 to f64
  %v33 = llvm.fcmp "une" %v30, %v32 : f64
  %v34 = llvm.mlir.constant(1 : i64) : i64
  %v35 = llvm.mlir.constant(0 : i64) : i64
  %v36 = llvm.select %v33, %v34, %v35 : i1, i64
  llvm.br ^L5(%v36: i64)
^L3:
  %v37 = llvm.mlir.constant(-281474976710654 : i64) : i64
  %v38 = llvm.icmp "eq" %a0, %v37 : i64
  llvm.cond_br %v38, ^L6, ^L8
^L8:
  %v39 = llvm.mlir.constant(-281474976710650 : i64) : i64
  %v40 = llvm.icmp "eq" %a0, %v39 : i64
  llvm.cond_br %v40, ^L7, ^L9
^L9:
  %v41 = llvm.mlir.constant(-281474976710652 : i64) : i64
  %v42 = llvm.icmp "eq" %a0, %v41 : i64
  llvm.cond_br %v42, ^L6, ^L4
^L6:
  %v43 = llvm.mlir.constant(0 : i64) : i64
  llvm.br ^L5(%v43: i64)
^L7:
  %v44 = llvm.mlir.constant(1 : i64) : i64
  llvm.br ^L5(%v44: i64)
^L4:
  %v45 = llvm.call @a_truthy_raw_slow(%a0) : (i64) -> i64
  llvm.br ^L5(%v45: i64)
^L5(%v13: i64):
  llvm.return %v13 : i64
}

llvm.func @a_pow_fast(%a0: i64, %a1: i64) -> i64 attributes {llvm.linkage = #llvm.linkage<linkonce_odr>} {
  %v0 = llvm.call @a_pow(%a0, %a1) : (i64, i64) -> i64
  llvm.return %v0 : i64
}

)MLIR";

}  // namespace fastpathmlir
}  // namespace a
