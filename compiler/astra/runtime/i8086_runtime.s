# i8086 bare-metal runtime subset for the native A backend.
# aval = NaN-boxed uint64: int = 0xFFFF000000000000 | ((i<<1)|1), i is
# a signed 47-bit value stored in the low 48 payload bits.
# cdecl: ret@0(%ebp+4), arg1@8 (8 bytes), arg2@16, arg3@24 ...
# returns: eax=lo32 edx=hi32 of aval.

    .section .text
    .globl a_runtime_init
# Zero [_edata, _end): .bss is SHT_NOBITS so the flat loader never writes
# it — without this it contains stale RAM instead of the zero-init the
# language expects. _edata/_end are linker-provided symbols; the range also
# covers the align gap between .data and .bss (harmless to clear).
a_runtime_init:
    pushl %ebp
    movl %esp, %ebp
    movl $_edata, %esi
    movl $_end, %edi
    cmpl %edi, %esi
    jae .Lrt_bss_done
.Lbss_zero:
    movl $0, (%esi)
    addl $4, %esi
    cmpl %edi, %esi
    jb .Lbss_zero
.Lrt_bss_done:
    # The bss sweep above can cover the .data tail on some link layouts,
    # wiping heap_cur/heap_end to 0 (which makes every .Lalloc fail and
    # return NULL -> A_NONE). Re-arm the bump allocator unconditionally;
    # init runs once before any allocation.
    movl $0x80000, heap_cur
    movl $0x8F000, heap_end
    popl %ebp
    ret

# .Lunbox: esi=p_lo edi=p_hi32(aval hi) -> esi=i_lo edi=i_hi (sign-extended)
# clobbers eax,ecx,edx
.Lunbox:
    movl %edi, %eax
    andl $0xFFFF, %eax        # eax = payload bits 32..47
    movl %eax, %ecx
    andl $1, %ecx
    shll $31, %ecx            # i bit31 = payload bit32
    movl %esi, %edx
    shrl $1, %edx             # payload_lo >> 1
    orl %ecx, %edx            # i_lo
    movl %eax, %ecx
    shrl $1, %ecx             # i bits 32..46
    testl $0x8000, %eax       # payload bit47 = i sign
    jz 1f
    orl $0xFFFF8000, %ecx
1:  movl %edx, %esi
    movl %ecx, %edi
    ret

# .Lrebox: esi=i_lo edi=i_hi -> eax=p_lo edx=aval_hi32
# clobbers ecx only (eax,edx are outputs)
.Lrebox:
    movl %esi, %eax
    movl %esi, %ecx
    shrl $31, %ecx            # carry = i bit31 -> payload bit32
    shll $1, %eax
    orl $1, %eax              # p_lo
    movl %edi, %edx
    shll $1, %edx             # i_hi<<1 (drops sign bits, keeps bits32..46)
    addl %ecx, %edx           # | carry into bit0
    andl $0xFFFF, %edx
    orl $0xFFFF0000, %edx     # aval hi32 = tag | p_hi16
    ret

# int fast-path check + unbox for binary ops.
# On entry (a_add_slow/a_sub_slow): ebp frame set. Checks a@8,b@16 are
# tagged ints; if so jumps to .Ldo with esi:edi = a_int, and pushes b ptr.
# Too complex inline; each op inlines its own checks (below).

# a_add_slow(aval a, aval b) -> aval
    .globl a_add_slow
a_add_slow:
    pushl %ebp
    movl %esp, %ebp
    # a tagged? hi&0xFFFF0000==0xFFFF0000 && lo&1
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .La_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .La_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .La_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .La_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx            # ecx = a_hi
    popl %edx            # edx = a_lo
    addl %edx, %esi      # b_lo += a_lo
    adcl %ecx, %edi      # b_hi += a_hi
    call .Lrebox
    popl %ebp
    ret
.La_none:
    # str + str -> concat (T_STR objects only)
    movl 12(%ebp), %eax        # a hi
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .La_ret_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jnz .La_ret_none
    cmpl $2, %eax
    je .La_ret_none
    cmpl $4, %eax
    je .La_ret_none
    cmpl $6, %eax
    je .La_ret_none
    testl $0xF, %eax
    jnz .La_ret_none
    testl %eax, %eax
    jz .La_ret_none
    cmpl $1, (%eax)
    jne .La_ret_none
    movl %eax, %esi            # A ptr
    movl 20(%ebp), %eax        # b hi
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .La_ret_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jnz .La_ret_none
    cmpl $2, %eax
    je .La_ret_none
    cmpl $4, %eax
    je .La_ret_none
    cmpl $6, %eax
    je .La_ret_none
    testl $0xF, %eax
    jnz .La_ret_none
    testl %eax, %eax
    jz .La_ret_none
    cmpl $1, (%eax)
    jne .La_ret_none
    movl %eax, %edi            # B ptr
    call .Lstrcat
    popl %ebp
    ret
.La_ret_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_sub_slow(aval a, aval b) -> aval
    .globl a_sub_slow
a_sub_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Ls_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Ls_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Ls_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Ls_none
    # want l - r: unbox r (arg2) first and save it, then l lands in esi:edi.
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    popl %ecx            # ecx = r_hi
    popl %edx            # edx = r_lo
    subl %edx, %esi      # l_lo -= r_lo
    sbbl %ecx, %edi      # l_hi -= r_hi
    call .Lrebox
    popl %ebp
    ret
.Ls_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_mul_slow / a_div_slow / a_mod_slow : int fast path via 32-bit mul/div
# (|i| < 2^31 only; larger falls back to A_NONE for now)
    .globl a_mul_slow
a_mul_slow:
    pushl %ebp
    movl %esp, %ebp
    # tagged checks (same pattern)
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lm_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lm_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lm_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lm_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx            # ecx = a_hi
    popl %edx            # edx = a_lo
    # b currently esi:edi = b_lo:b_hi ; require s32 range: hi in {0,-1}
    testl %edi, %edi
    jnz .Lm_big
    cmpl $0xFFFFFFFF, %esi
    je .Lm_big
    testl %ecx, %ecx
    jnz .Lm_chkneg_a
    jmp .Lm_mul
.Lm_chkneg_a:
    cmpl $0xFFFFFFFF, %ecx
    jne .Lm_big
.Lm_mul:
    # edx = a_lo (s32), esi = b_lo (s32); one-operand imul -> edx:eax = s64 product
    movl %edx, %eax
    imull %esi            # edx:eax = eax * esi
    movl %eax, %esi
    movl %edx, %edi
    call .Lrebox
    popl %ebp
    ret
.Lm_big:
.Lm_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_div_slow / a_mod_slow : signed 32-bit idiv for small ints
    .globl a_div_slow
a_div_slow:
    pushl %ebp
    movl %esp, %ebp
    call .Ldivmod
    # eax=quot edx=rem ; want quot -> esi:i_lo
    movl %eax, %esi
    movl %eax, %edi
    sarl $31, %edi
    call .Lrebox
    popl %ebp
    ret
    .globl a_mod_slow
a_mod_slow:
    pushl %ebp
    movl %esp, %ebp
    call .Ldivmod
    movl %edx, %esi
    movl %edx, %edi
    sarl $31, %edi
    call .Lrebox
    popl %ebp
    ret
# .Ldivmod: tagged-int checks + unbox + idiv; returns eax=quot edx=rem.
# clobbers everything except ebp. On failure jumps to .Ld_fail_ret via
# longjmp-style: sets eax=edx=marker? Simpler: returns quot=rem=0x80000000
# sentinel is ugly — instead .Ldivmod does its own frame and returns
# status in a flag. We instead duplicate: .Ldivmod assumes checks done?
# Cleanest: .Ldivmod performs checks and on failure returns with
# .Ldiv_bad set; callers test it.
    .data
    .globl .Ldiv_bad
.Ldiv_bad: .byte 0
    .text
.Ldivmod:
    movb $0, .Ldiv_bad
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Ld_bad
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Ld_bad
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Ld_bad
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Ld_bad
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx            # a_hi
    popl %edx            # a_lo
    # s32 range: hi in {0,-1}
    testl %ecx, %ecx
    jz .Ld_chk_b
    cmpl $0xFFFFFFFF, %ecx
    jne .Ld_bad
.Ld_chk_b:
    testl %edi, %edi
    jz .Ld_go
    cmpl $0xFFFFFFFF, %edi
    jne .Ld_bad
.Ld_go:
    testl %esi, %esi
    jz .Ld_bad           # div by zero
    movl %edx, %eax
    cdq
    idivl %esi           # eax=quot edx=rem (signed)
    ret
.Ld_bad:
    movb $1, .Ldiv_bad
    xorl %eax, %eax
    xorl %edx, %edx
    ret

# a_truthy_raw_slow(aval a) -> plain u64 0 or 1 (NOT an aval!)
# Callers test the raw result with jz/jnz, matching the C runtime which
# returns (aval)(truthy(v) ? 1 : 0). Returning A_TRUE/A_FALSE avals here
# makes every condition permanently true (both are nonzero avals).
    .globl a_truthy_raw_slow
a_truthy_raw_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax          # lo
    movl 12(%ebp), %edx         # hi
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lt_untagged            # untagged (double bits): nonzero -> true
    testl $1, %eax
    jnz .Lt_int                 # tagged int
    # even tagged payload: A_NONE=2, A_FALSE=4, A_TRUE=6, else heap ptr
    cmpl $2, %eax
    je .Lt_zero
    cmpl $4, %eax
    je .Lt_zero
    cmpl $6, %eax
    je .Lt_one
    testl $0xF, %eax
    jz .Lt_obj                  # 16-aligned -> heap object
    jmp .Lt_zero
.Lt_int:
    # int i == 0 iff aval == 0xFFFF000000000001 (payload = (i<<1)|1)
    cmpl $1, %eax
    jne .Lt_one
    cmpl $0xFFFF0000, %edx
    je .Lt_zero
    jmp .Lt_one
.Lt_obj:                       # eax = obj ptr; T_STR/T_LIST/T_DICT/T_SET: len@4
    movl %eax, %esi
    movl (%esi), %ecx
    cmpl $1, %ecx               # T_STR / T_LIST / T_TUPLE / T_DICT / T_SET
    je .Lt_len
    cmpl $2, %ecx
    je .Lt_len
    cmpl $3, %ecx
    je .Lt_len
    cmpl $4, %ecx
    je .Lt_len
    cmpl $5, %ecx
    je .Lt_len
    jmp .Lt_one
.Lt_len:
    movl 4(%esi), %ecx
    testl %ecx, %ecx
    jnz .Lt_one
    jmp .Lt_zero
.Lt_untagged:
    orl %edx, %eax
    jnz .Lt_one
    jmp .Lt_zero
.Lt_one:
    movl $1, %eax
    xorl %edx, %edx
    popl %ebp
    ret
.Lt_zero:
    xorl %eax, %eax
    xorl %edx, %edx
    popl %ebp
    ret

# a_not(aval v) -> aval A_TRUE/A_FALSE  (logical not)
    .globl a_not
a_not:
    pushl %ebp
    movl %esp, %ebp
    # cdecl aval arg: push hi word first so lo lands at callee's 8(%ebp)
    movl 12(%ebp), %eax
    pushl %eax
    movl 8(%ebp), %eax
    pushl %eax
    call a_truthy_raw_slow
    addl $8, %esp              # eax = 0/1
    testl %eax, %eax
    jnz .Lnot_f
    movl $0x00000006, %eax      # A_TRUE
    movl $0xFFFF0000, %edx
    jmp .Lnot_d
.Lnot_f:
    movl $0x00000004, %eax      # A_FALSE
    movl $0xFFFF0000, %edx
.Lnot_d:
    popl %ebp
    ret

# a_neg_slow(aval v) -> aval : unary minus on a tagged int.
# -i == ~i + 1 on the unboxed signed 64-bit value, then rebox.
# |i| = 2^46 (min int) overflows the 47-bit payload and wraps — the C
# runtime would promote to bigint; acceptable gap for the i8086 subset.
    .globl a_neg_slow
a_neg_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lneg_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lneg_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox               # esi=i_lo edi=i_hi
    notl %esi
    notl %edi
    addl $1, %esi
    adcl $0, %edi
    call .Lrebox
    popl %ebp
    ret
.Lneg_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_bnot_slow(aval v) -> aval : bitwise ~ on a tagged int.
# ~i stays inside the 47-bit signed range (involution), so no overflow.
    .globl a_bnot_slow
a_bnot_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbnot_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lbnot_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox               # esi=i_lo edi=i_hi
    notl %esi
    notl %edi
    call .Lrebox
    popl %ebp
    ret
.Lbnot_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_eq_slow / a_ne_slow / a_lt_slow : int compares
    .globl a_eq_slow
a_eq_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    cmpl 16(%ebp), %eax
    jne .Lc_false
    movl 12(%ebp), %eax
    cmpl 20(%ebp), %eax
    jne .Lc_false
    jmp .Lc_true
.Lc_false:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lc_true:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_ne_slow
a_ne_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    cmpl 16(%ebp), %eax
    jne .Ln_true
    movl 12(%ebp), %eax
    cmpl 20(%ebp), %eax
    jne .Ln_true
    jmp .Ln_false
.Ln_true:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Ln_false:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_lt_slow(aval a, aval b): signed 64-bit compare of unboxed ints;
# non-int -> A_NONE
    .globl a_lt_slow
a_lt_slow:
    pushl %ebp
    movl %esp, %ebp
    # both tagged ints?
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Llt_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Llt_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Llt_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Llt_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    # a = edx:ecx? pops: pushed esi,edi -> ecx=edi(hi) edx=esi(lo). ugh ordering again:
    # pushed a_lo then a_hi: popl %ecx = a_hi, popl %edx = a_lo
    # b currently esi:edi
    # signed 64-bit compare a < b: a_hi=ecx a_lo=edx, b_hi=edi b_lo=esi.
    # Compare high words signed first, then low words unsigned.
    cmpl %edi, %ecx          # a_hi - b_hi
    jl .Llt_yes
    jg .Llt_no
    cmpl %esi, %edx          # a_lo - b_lo (unsigned)
    jb .Llt_yes
    jmp .Llt_no
.Llt_yes:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Llt_no:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Llt_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_gt_slow / a_le_slow / a_ge_slow : signed 64-bit int compares
    .globl a_gt_slow
a_gt_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lgt_slow_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lgt_slow_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lgt_slow_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lgt_slow_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    cmpl %edi, %ecx
    jg .Lgt_slow_yes
    jl .Lgt_slow_no
    cmpl %esi, %edx
    ja .Lgt_slow_yes
    jmp .Lgt_slow_no
.Lgt_slow_yes:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lgt_slow_no:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lgt_slow_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_le_slow
a_le_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lle_slow_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lle_slow_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lle_slow_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lle_slow_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    cmpl %edi, %ecx
    jl .Lle_slow_yes
    jg .Lle_slow_no
    cmpl %esi, %edx
    jbe .Lle_slow_yes
    jmp .Lle_slow_no
.Lle_slow_yes:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lle_slow_no:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lle_slow_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_ge_slow
a_ge_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lge_slow_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lge_slow_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lge_slow_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lge_slow_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    cmpl %edi, %ecx
    jg .Lge_slow_yes
    jl .Lge_slow_no
    cmpl %esi, %edx
    jae .Lge_slow_yes
    jmp .Lge_slow_no
.Lge_slow_yes:
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lge_slow_no:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lge_slow_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_bi_call(id, n, argv) -> aval ; id 0 = print
    .globl a_bi_call
a_bi_call:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax        # id hi32 must be 0
    testl %eax, %eax
    jnz .Lb_none
    movl 8(%ebp), %eax         # id
    cmpl $0, %eax
    je .Lb_args
    cmpl $1, %eax
    je .Lb_args                # eprint -> serial too
    cmpl $2, %eax
    je .Lb_blen
    cmpl $3, %eax
    je .Lb_bstr
    cmpl $4, %eax
    je .Lb_bint
    cmpl $6, %eax
    je .Lb_bbool
    cmpl $7, %eax
    je .Lb_babs
    cmpl $27, %eax
    je .Lb_brange
    cmpl $53, %eax
    je .Lb_brepr             # repr -> a_repr (collections -> "[..]" etc)
    jmp .Lb_none
.Lb_args:
    xorl %ecx, %ecx          # i = 0
.Lb_loop:
    cmpl 16(%ebp), %ecx       # n (8-byte arg at +16)
    jge .Lb_nl
    testl %ecx, %ecx          # C printv: ' ' separator between args
    jz .Lb_nosep
    movb $' ', %al
    call .Lputc
.Lb_nosep:
    movl 24(%ebp), %eax       # argv ptr (4-byte arg at +24)
    movl %ecx, %edx
    shll $3, %edx
    addl %edx, %eax          # argv + i*8
    movl %eax, %esi          # esi = aval ptr
    # tagged int? (hi&0xFFFF0000==0xFFFF0000 && lo&1)
    movl 4(%esi), %edi
    andl $0xFFFF0000, %edi
    cmpl $0xFFFF0000, %edi
    jne .Lb_q
    movl (%esi), %ebx
    testl $1, %ebx
    jz .Lb_even
    pushl %ecx              # .Lb_printint clobbers ecx (loop counter)
    call .Lb_printint       # clobbers all caller-saved regs
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_q:
    movb $'?', %al
    call .Lputc
    incl %ecx
    jmp .Lb_loop
.Lb_even:                   # even low word: none/false/true, ptr, or '?'
    cmpl $2, %ebx
    je .Lb_cnone
    cmpl $4, %ebx
    je .Lb_cfalse
    cmpl $6, %ebx
    je .Lb_ctrue
    testl $0xF, %ebx          # 16-aligned nonzero -> heap object ptr
    jz .Lb_cstr
    jmp .Lb_q
.Lb_cstr:                    # ebx = obj ptr; AObj{type,len,cap}+data@16
    movl %ebx, %edi           # edi = obj base (edi was masked tag, free)
    movl (%edi), %eax
    cmpl $1, %eax             # T_STR
    je .Lb_cs_str
    cmpl $12, %eax            # T_CHAR
    je .Lb_cs_str
    # other heap object -> repr via .Lb_paval(&argv[i])
    pushl %ecx
    movl 24(%ebp), %esi       # argv base
    movl %ecx, %edx
    shll $3, %edx
    addl %edx, %esi           # &argv[i]
    call .Lb_paval
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_cs_str:
    movl 4(%edi), %ebx        # len
    movl %edi, %esi
    addl $16, %esi            # data ptr (avoid lea: addr32 prefix quirks)
    pushl %ecx
    call .Lb_printn
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_cnone:
    pushl %ecx
    movl $str_none, %esi
    movl $4, %ebx
    call .Lb_printn
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_cfalse:
    pushl %ecx
    movl $str_false, %esi
    movl $5, %ebx
    call .Lb_printn
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_ctrue:
    pushl %ecx
    movl $str_true, %esi
    movl $4, %ebx
    call .Lb_printn
    popl %ecx
    incl %ecx
    jmp .Lb_loop
.Lb_nl:
    movb $10, %al
    call .Lputc
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
# ---- builtin handlers (argv ptr @24(%ebp), avals 8B each) ------------------
.Lb_xret:
    popl %ebp
    ret
.Lb_blen:
    movl 24(%ebp), %eax
    pushl 4(%eax)
    pushl (%eax)
    call a_len
    addl $8, %esp
    jmp .Lb_xret
.Lb_bstr:
    movl 24(%ebp), %eax
    pushl 4(%eax)
    pushl (%eax)
    call a_str
    addl $8, %esp
    jmp .Lb_xret
.Lb_brepr:
    movl 24(%ebp), %eax
    pushl 4(%eax)
    pushl (%eax)
    call a_repr
    addl $8, %esp
    jmp .Lb_xret
.Lb_bbool:
    movl 24(%ebp), %eax
    pushl 4(%eax)
    pushl (%eax)
    call a_truthy_raw_slow
    addl $8, %esp
    testl %eax, %eax
    jnz .Lb_bb_f
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    jmp .Lb_xret
.Lb_bb_f:
    movl $0x00000004, %eax
    movl $0xFFFF0000, %edx
    jmp .Lb_xret
.Lb_babs:
    movl 24(%ebp), %ecx
    movl (%ecx), %eax
    movl 4(%ecx), %edx
    call .Labs
    jmp .Lb_xret
.Lb_brange:
    movl 16(%ebp), %eax         # n lo
    cmpl $1, %eax
    je .Lbr1
    cmpl $2, %eax
    je .Lbr2
    cmpl $3, %eax
    je .Lbr3
    jmp .Lb_none
.Lbr3:
    movl 24(%ebp), %ecx         # argv
    pushl 20(%ecx)              # step hi
    pushl 16(%ecx)              # step lo
    pushl 12(%ecx)              # hi hi
    pushl 8(%ecx)               # hi lo
    pushl 4(%ecx)               # lo hi
    pushl (%ecx)                # lo lo
    call a_range_new
    addl $24, %esp
    jmp .Lb_xret
.Lbr2:
    movl 24(%ebp), %ecx
    pushl $0xFFFF0000           # step = mkint(1)
    pushl $3
    pushl 12(%ecx)              # hi = argv[1]
    pushl 8(%ecx)
    pushl 4(%ecx)               # lo = argv[0]
    pushl (%ecx)
    call a_range_new
    addl $24, %esp
    jmp .Lb_xret
.Lbr1:
    movl 24(%ebp), %ecx
    pushl $0xFFFF0000           # step = mkint(1)
    pushl $3
    pushl 4(%ecx)               # hi = argv[0]
    pushl (%ecx)
    pushl $0xFFFF0000           # lo = mkint(0)
    pushl $1
    call a_range_new
    addl $24, %esp
    jmp .Lb_xret
.Lb_bint:
    movl 24(%ebp), %ecx         # argv
    movl (%ecx), %eax           # lo
    movl 4(%ecx), %edx          # hi
    # already tagged int? -> return as-is
    movl %edx, %esi
    andl $0xFFFF0000, %esi
    cmpl $0xFFFF0000, %esi
    jne .Lbi_maybe
    testl $1, %eax
    jz .Lbi_maybe
    jmp .Lb_xret
.Lbi_maybe:
    # T_STR? -> parse decimal
    movl %edx, %esi
    andl $0xFFFF0000, %esi
    cmpl $0xFFFF0000, %esi
    jne .Lb_none
    testl $1, %eax
    jnz .Lb_none
    cmpl $2, %eax
    je .Lb_none
    cmpl $4, %eax
    je .Lb_none
    cmpl $6, %eax
    je .Lb_none
    testl $0xF, %eax
    jnz .Lb_none
    testl %eax, %eax
    jz .Lb_none
    cmpl $1, (%eax)
    jne .Lb_none
    movl %eax, %esi             # str ptr
    movl 4(%esi), %ecx          # len
    movl %esi, %edi
    addl $16, %edi              # p
    movl %edi, %ebx
    addl %ecx, %ebx             # end
    cmpl %ebx, %edi
    jae .Lb_none                # empty
    xorl %edx, %edx             # neg flag
    cmpb $0x2d, (%edi)          # '-'
    jne .Lbi_dig
    movl $1, %edx
    incl %edi
.Lbi_dig:
    pushl $0                    # val slot [esp]
    pushl $0                    # neg slot [esp+4]
    movl %edx, 4(%esp)
    cmpl %ebx, %edi
    jae .Lbi_bad
.Lbi_dloop:
    cmpl %ebx, %edi
    jae .Lbi_dend
    movzbl (%edi), %ecx
    subl $0x30, %ecx
    cmpl $9, %ecx
    ja .Lbi_bad
    movl (%esp), %eax
    movl %eax, %edx
    shll $1, %edx               # 2v
    shll $3, %eax               # 8v
    addl %edx, %eax             # 10v
    addl %ecx, %eax             # +digit
    movl %eax, (%esp)
    incl %edi
    jmp .Lbi_dloop
.Lbi_dend:
    movl (%esp), %eax           # magnitude
    movl 4(%esp), %ecx
    testl %ecx, %ecx
    jz .Lbi_box
    negl %eax
.Lbi_box:
    movl %eax, %esi
    movl %eax, %ecx
    sarl $31, %ecx
    movl %ecx, %edi             # sign extend
    call .Lrebox
    addl $8, %esp
    jmp .Lb_xret
.Lbi_bad:
    addl $8, %esp
    jmp .Lb_none
.Lb_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# .Lb_printint: esi -> aval; prints decimal int or '?'. |v| < 2^31.
# clobbers eax,ebx,ecx,edx,esi,edi
.Lb_printint:
    movl (%esi), %ebx        # p_lo
    movl 4(%esi), %edi       # p_hi32
    movl %edi, %eax
    andl $0xFFFF, %eax       # p_hi16
    movl %ebx, %edx
    shrl $1, %edx            # p_lo>>1
    movl %eax, %ecx
    andl $1, %ecx
    shll $31, %ecx
    orl %ecx, %edx           # edx = i_lo
    movl %eax, %ecx
    shrl $1, %ecx            # i_hi base
    testl $0x8000, %eax      # payload bit47 = i sign
    jz 1f
    orl $0xFFFF8000, %ecx
1:  # ecx = i_hi, edx = i_lo
    testl %ecx, %ecx
    jz .Lpi_nonneg
    cmpl $0xFFFFFFFF, %ecx
    jne .Lpi_q
    testl $0x80000000, %edx
    jz .Lpi_q
    movb $'-', %al
    call .Lputc
    movl %edx, %eax
    cmpl $0x80000000, %eax
    je .Lpi_q
    negl %eax
    jmp .Lpi_div
.Lpi_nonneg:
    testl $0x80000000, %edx
    jnz .Lpi_q
    movl %edx, %eax
.Lpi_div:
    subl $16, %esp
    movl %esp, %ebx
    addl $15, %ebx
    movl %ebx, %edi          # end = esp+15
.Lpi_dloop:
    xorl %edx, %edx
    movl $10, %ecx
    divl %ecx                # eax=quot edx=rem (eax < 2^31)
    addl $0x30, %edx
    movb %dl, (%ebx)
    decl %ebx
    testl %eax, %eax
    jnz .Lpi_dloop
.Lpi_prloop:
    incl %ebx
    cmpl %edi, %ebx
    ja .Lpi_done
    movb (%ebx), %al
    call .Lputc
    jmp .Lpi_prloop
.Lpi_done:
    addl $16, %esp
    ret
.Lpi_q:
    movb $'?', %al
    call .Lputc
    ret

.Lb_printn:                 # esi=ptr ebx=len; clobbers eax,esi,ebx
.Lpn_loop:
    testl %ebx, %ebx
    jz .Lpn_done
    movb (%esi), %al
    call .Lputc
    incl %esi
    decl %ebx
    jmp .Lpn_loop
.Lpn_done:
    ret

.Lputc:                     # al -> COM1; preserves edx (callers keep i_lo there)
    pushl %edx
    movw $0x3F8, %dx
    outb %al, %dx
    popl %edx
    ret

# .Lb_paval: esi -> aval (8 bytes); prints repr (subset). Recursive for
# collections. clobbers eax,ebx,ecx,edx,esi,edi; preserves ebp.
.Lb_paval:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] aval ptr
    movl (%esi), %eax           # lo
    movl 4(%esi), %edx          # hi
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lpv_q                  # untagged (double) -> '?'
    testl $1, %eax
    jnz .Lpv_int
    cmpl $2, %eax
    je .Lpv_none
    cmpl $4, %eax
    je .Lpv_false
    cmpl $6, %eax
    je .Lpv_true
    testl $0xF, %eax
    jnz .Lpv_q
    testl %eax, %eax
    jz .Lpv_q
    # heap object: type@0
    movl %eax, %ebx
    movl (%ebx), %ecx
    cmpl $1, %ecx               # T_STR
    je .Lpv_str
    cmpl $12, %ecx              # T_CHAR
    je .Lpv_str
    cmpl $2, %ecx               # T_LIST
    je .Lpv_arr
    cmpl $3, %ecx               # T_TUPLE
    je .Lpv_arr
    cmpl $5, %ecx               # T_SET
    je .Lpv_set
    cmpl $4, %ecx               # T_DICT
    je .Lpv_dict
    cmpl $10, %ecx              # T_RESULT (some/ok)
    je .Lpv_opt
    jmp .Lpv_q
.Lpv_opt:
    cmpl $0, 16(%ebx)           # ok?
    je .Lpv_q
    movl %ebx, %esi
    addl $24, %esi              # &value aval
    call .Lb_paval
    jmp .Lpv_out
.Lpv_int:
    movl -4(%ebp), %esi
    call .Lb_printint
    jmp .Lpv_out
.Lpv_none:
    movl $str_none, %esi
    movl $4, %ebx
    call .Lb_printn
    jmp .Lpv_out
.Lpv_false:
    movl $str_false, %esi
    movl $5, %ebx
    call .Lb_printn
    jmp .Lpv_out
.Lpv_true:
    movl $str_true, %esi
    movl $4, %ebx
    call .Lb_printn
    jmp .Lpv_out
.Lpv_str:                      # ebx = obj; print "..." quoted
    movb $'"', %al
    call .Lputc
    movl 4(%ebx), %ebx          # len (bytes)
    movl -4(%ebp), %eax
    movl (%eax), %esi           # obj ptr
    addl $16, %esi              # data
    call .Lb_printn
    movb $'"', %al
    call .Lputc
    jmp .Lpv_out
.Lpv_arr:                      # ebx = T_LIST/T_TUPLE; "[a, b]"
    pushl %ebx                  # [ebp-8] obj
    pushl $0                    # [ebp-12] i
    movb $'[', %al
    call .Lputc
.Lpva_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lpva_close
    testl %eax, %eax
    jz .Lpva_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lb_printn
.Lpva_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx         # items base
    movl -12(%ebp), %eax
    shll $3, %eax
    addl %eax, %ecx             # &items[i] aval
    movl %ecx, %esi
    call .Lb_paval
    incl -12(%ebp)
    jmp .Lpva_loop
.Lpva_close:
    movb $']', %al
    call .Lputc
    jmp .Lpv_out
.Lpv_set:                      # ebx = T_SET; "{a, b}"
    pushl %ebx
    pushl $0
    movb $'{', %al
    call .Lputc
.Lpvs_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lpvs_close
    testl %eax, %eax
    jz .Lpvs_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lb_printn
.Lpvs_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx
    movl -12(%ebp), %eax
    shll $3, %eax
    addl %eax, %ecx
    movl %ecx, %esi
    call .Lb_paval
    incl -12(%ebp)
    jmp .Lpvs_loop
.Lpvs_close:
    movb $'}', %al
    call .Lputc
    jmp .Lpv_out
.Lpv_dict:                     # ebx = T_DICT; "{k: v}"
    pushl %ebx                  # [ebp-8] obj
    pushl $0                    # [ebp-12] i
    movb $'{', %al
    call .Lputc
.Lpvd_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lpvd_close
    testl %eax, %eax
    jz .Lpvd_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lb_printn
.Lpvd_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx         # pairs base
    movl -12(%ebp), %eax
    shll $4, %eax
    addl %eax, %ecx             # &pair[i] (k aval @0, v aval @8)
    movl %ecx, %esi
    call .Lb_paval
    movl $str_colon, %esi
    movl $2, %ebx
    call .Lb_printn
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx
    movl -12(%ebp), %eax
    shll $4, %eax
    addl $8, %eax
    addl %eax, %ecx             # &pair[i].v
    movl %ecx, %esi
    call .Lb_paval
    incl -12(%ebp)
    jmp .Lpvd_loop
.Lpvd_close:
    movb $'}', %al
    call .Lputc
    jmp .Lpv_out
.Lpv_q:
    movb $'?', %al
    call .Lputc
.Lpv_out:
    movl %ebp, %esp
    popl %ebp
    ret

    .section .rodata
str_none:  .ascii "none"
str_false: .ascii "false"
str_true:  .ascii "true"
str_comma: .ascii ", "
str_colon: .ascii ": "

    .section .data
    .globl serial_init
serial_init: .byte 0
    # Bump-allocator arena: flat physical addresses (DS=0). The heap lives
    # in unused RAM below the stack segment (SS=0x9000 => 0x90000..0x9F000);
    # .data VMA == physical load address so absolute refs are correct.
    .globl heap_cur
    .globl heap_end
heap_cur: .long 0x80000
heap_end: .long 0x8F000

# ---- shared helpers -------------------------------------------------------
# Object layouts (32-bit flat pointers, DS=0):
#   AStr   : type@0,len@4,cap@8(32b used),data@16          (existing)
#   AArr   : type@0,len@4,cap@8,items_ptr@16 -> aval[8B]   (list=2,tuple=3,set=5)
#   ADict  : type@0,len@4(pair count),cap@8,pairs_ptr@16 -> {k aval,v aval}[16B]
#   ARange : type@0,len@4,cap@8,lo i64@16,hi i64@24,step i64@32
#   AIter  : type@0,len@4,cap@8,src aval@16,idx u32@24,
#            cur i64@32,end i64@40,step i64@48
#   AEnv   : type@0,len@4(n),slots aval[]@16
#   AFnon  : type@0,len@4,cap@8,entry u32@16,env aval@20,name@28,builtin@32
#   AResult: type@0,len@4,cap@8,ok@16,is_option@20,value aval@24

# .Lalloc: ebx = size in bytes -> eax = 16-aligned heap ptr, or 0.
# clobbers eax,ecx,edx,edi ; preserves ebx,esi,ebp
.Lalloc:
    movl %ebx, %eax
    addl $15, %eax
    andl $0xFFFFFFF0, %eax
    movl heap_cur, %edi
    movl heap_end, %ecx
    movl %edi, %edx
    addl %eax, %edx
    cmpl %ecx, %edx
    ja .Lal_fail
    movl %edx, heap_cur
    movl %edi, %eax
    ret
.Lal_fail:
    xorl %eax, %eax
    ret

# .Lobjptr: eax = aval lo32, edx = aval hi32 -> esi = heap obj ptr or 0.
# Tagged ints (lo&1), A_NONE/A_FALSE/A_TRUE (lo 2/4/6) and untagged values
# yield 0. clobbers ecx.
.Lobjptr:
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lop_no
    testl $1, %eax
    jnz .Lop_no
    cmpl $2, %eax
    je .Lop_no
    cmpl $4, %eax
    je .Lop_no
    cmpl $6, %eax
    je .Lop_no
    testl $0xF, %eax
    jnz .Lop_no
    testl %eax, %eax
    jz .Lop_no
    movl %eax, %esi
    ret
.Lop_no:
    xorl %esi, %esi
    ret

# .Lmksmall: eax = u32 value (< 2^31) -> eax:edx tagged-int aval. clobbers ecx,esi,edi
.Lmksmall:
    movl %eax, %esi
    xorl %edi, %edi
    call .Lrebox
    ret

# .Leqkey: esi -> aval A, edi -> aval B -> eax = 1/0.
# Identical avals are equal; differing avals are equal iff both are T_STR
# heap objects with identical bytes (matches values_equal for our subset).
# Preserves esi,edi. clobbers eax,ebx,ecx,edx.
.Leqkey:
    pushl %esi
    pushl %edi
    movl (%esi), %eax
    cmpl %eax, (%edi)
    jne .Lek_maybe
    movl 4(%esi), %eax
    cmpl %eax, 4(%edi)
    je .Lek_yes
.Lek_maybe:
    # A heap T_STR? ebx=a_lo ecx=a_hi
    movl (%esi), %ebx
    movl 4(%esi), %ecx
    movl %ecx, %edx
    andl $0xFFFF0000, %edx
    cmpl $0xFFFF0000, %edx
    jne .Lek_no
    testl $1, %ebx
    jnz .Lek_no
    cmpl $2, %ebx
    je .Lek_no
    cmpl $4, %ebx
    je .Lek_no
    cmpl $6, %ebx
    je .Lek_no
    testl $0xF, %ebx
    jnz .Lek_no
    testl %ebx, %ebx
    jz .Lek_no
    cmpl $1, (%ebx)
    jne .Lek_no
    # B heap T_STR? edx=b_lo, ecx reused for hi check via eax
    movl (%edi), %edx
    movl 4(%edi), %eax
    movl %eax, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lek_no
    testl $1, %edx
    jnz .Lek_no
    cmpl $2, %edx
    je .Lek_no
    cmpl $4, %edx
    je .Lek_no
    cmpl $6, %edx
    je .Lek_no
    testl $0xF, %edx
    jnz .Lek_no
    testl %edx, %edx
    jz .Lek_no
    cmpl $1, (%edx)
    jne .Lek_no
    # both T_STR: ebx=A ptr, edx=B ptr; compare len then bytes
    movl 4(%ebx), %ecx
    movl 4(%edx), %eax
    cmpl %ecx, %eax
    jne .Lek_no
    movl %ebx, %esi
    addl $16, %esi           # A data
    movl %edx, %edi
    addl $16, %edi           # B data
.Lek_bloop:
    testl %ecx, %ecx
    jz .Lek_yes
    movb (%esi), %al
    cmpb %al, (%edi)
    jne .Lek_no
    incl %esi
    incl %edi
    decl %ecx
    jmp .Lek_bloop
.Lek_yes:
    movl $1, %eax
    jmp .Lek_pop
.Lek_no:
    xorl %eax, %eax
.Lek_pop:
    popl %edi
    popl %esi
    ret

# .Larr_ensure: esi = AArr-like obj; grows so cap > len (8-byte aval elems).
# eax = 1 ok / 0 nomem. Preserves esi,ebp. clobbers eax,ebx,ecx,edx,edi.
.Larr_ensure:
    movl 4(%esi), %eax
    cmpl 8(%esi), %eax
    jb .Lae_ok
    movl 8(%esi), %ebx        # old cap (elems)
    testl %ebx, %ebx
    jz .Lae_nc
    addl %ebx, %ebx           # *2
    jmp .Lae_sz
.Lae_nc:
    movl $4, %ebx
.Lae_sz:
    movl %ebx, %eax
    shll $3, %eax
    movl %eax, %ebx           # ebx = bytes (alloc input); elems = ebx>>3
    call .Lalloc
    testl %eax, %eax
    jz .Lae_fail
    movl %eax, %edi           # dst cursor
    movl 16(%esi), %ecx       # old items
    movl 4(%esi), %edx
    shll $3, %edx             # nbytes
    movl %eax, 16(%esi)       # items = new
    movl %ebx, %eax
    shrl $3, %eax
    movl %eax, 8(%esi)        # cap = newcap elems
.Lae_cp:
    testl %edx, %edx
    jz .Lae_done
    movl (%ecx), %ebx
    movl %ebx, (%edi)
    movl 4(%ecx), %ebx
    movl %ebx, 4(%edi)
    addl $8, %ecx
    addl $8, %edi
    subl $8, %edx
    jmp .Lae_cp
.Lae_done:
    movl $1, %eax
    ret
.Lae_fail:
    xorl %eax, %eax
    ret
.Lae_ok:
    movl $1, %eax
    ret

# .Ldict_ensure: esi = ADict obj; grows pair capacity (16B pairs).
# Same contract as .Larr_ensure.
.Ldict_ensure:
    movl 4(%esi), %eax
    cmpl 8(%esi), %eax
    jb .Lde_ok
    movl 8(%esi), %ebx
    testl %ebx, %ebx
    jz .Lde_nc
    addl %ebx, %ebx
    jmp .Lde_sz
.Lde_nc:
    movl $4, %ebx
.Lde_sz:
    movl %ebx, %eax
    shll $4, %eax
    movl %eax, %ebx           # bytes; elems = ebx>>4
    call .Lalloc
    testl %eax, %eax
    jz .Lde_fail
    movl %eax, %edi
    movl 16(%esi), %ecx       # old pairs
    movl 4(%esi), %edx
    shll $4, %edx             # nbytes = len*16
    movl %eax, 16(%esi)
    movl %ebx, %eax
    shrl $4, %eax
    movl %eax, 8(%esi)
.Lde_cp:
    testl %edx, %edx
    jz .Lde_done
    movl (%ecx), %ebx
    movl %ebx, (%edi)
    movl 4(%ecx), %ebx
    movl %ebx, 4(%edi)
    movl 8(%ecx), %ebx
    movl %ebx, 8(%edi)
    movl 12(%ecx), %ebx
    movl %ebx, 12(%edi)
    addl $16, %ecx
    addl $16, %edi
    subl $16, %edx
    jmp .Lde_cp
.Lde_done:
    movl $1, %eax
    ret
.Lde_fail:
    xorl %eax, %eax
    ret
.Lde_ok:
    movl $1, %eax
    ret

# .Ldict_find: esi = dict obj, edi = flat ptr to key aval (8B)
# -> eax = pair index or 0xFFFFFFFF. Preserves esi,edi.
.Ldict_find:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] dict
    pushl %edi                  # [ebp-8] key ptr
    pushl $0                    # [ebp-12] i
.Ldf_loop:
    movl -12(%ebp), %eax
    movl -4(%ebp), %edx
    cmpl 4(%edx), %eax          # i vs len
    jae .Ldf_none
    movl -4(%ebp), %edx
    movl 16(%edx), %ecx         # pairs base
    movl -12(%ebp), %eax
    shll $4, %eax
    addl %eax, %ecx             # &pair[i].k
    movl %ecx, %esi
    movl -8(%ebp), %edi
    call .Leqkey
    testl %eax, %eax
    jnz .Ldf_hit
    incl -12(%ebp)
    jmp .Ldf_loop
.Ldf_hit:
    movl -12(%ebp), %eax
    jmp .Ldf_out
.Ldf_none:
    movl $0xFFFFFFFF, %eax
.Ldf_out:
    movl -8(%ebp), %edi         # restore saved regs (movl %ebp,%esp alone
    movl -4(%ebp), %esi         # would leave .Leqkey scratch in esi/edi)
    movl %ebp, %esp
    popl %ebp
    ret

# .Lstrcat: esi = T_STR ptr A, edi = T_STR ptr B -> eax:edx aval (new
# concatenated string) or A_NONE. clobbers eax,ebx,ecx,edx,edi (esi read-only).
.Lstrcat:
    pushl %edi                  # B ptr
    movl 4(%esi), %eax          # lenA
    pushl %eax
    movl 4(%edi), %ebx          # lenB
    movl %eax, %ecx
    addl %ebx, %ecx             # total
    pushl %ecx
    # stack: [esp]=total, [esp+4]=lenA, [esp+8]=B
    movl %ecx, %ebx
    addl $17, %ebx              # 16 hdr + total + 1 NUL
    call .Lalloc
    testl %eax, %eax
    jz .Lsc_fail
    movl %eax, %edx             # obj
    movl $1, (%edx)
    popl %eax                   # total
    movl %eax, 4(%edx)          # len
    incl %eax
    movl %eax, 8(%edx)          # cap
    movl $0, 12(%edx)
    movl %edx, %edi
    addl $16, %edi              # dst cursor
    movl %esi, %ecx
    addl $16, %ecx              # srcA cursor
    movl (%esp), %ebx           # lenA
.Lsc_ca:
    testl %ebx, %ebx
    jz .Lsc_cb
    movb (%ecx), %al
    movb %al, (%edi)
    incl %ecx
    incl %edi
    decl %ebx
    jmp .Lsc_ca
.Lsc_cb:
    movl 4(%edx), %ebx          # total len
    subl (%esp), %ebx           # - lenA = lenB
    movl 4(%esp), %ecx          # B ptr
    addl $16, %ecx
.Lsc_cbb:
    testl %ebx, %ebx
    jz .Lsc_done
    movb (%ecx), %al
    movb %al, (%edi)
    incl %ecx
    incl %edi
    decl %ebx
    jmp .Lsc_cbb
.Lsc_done:
    movb $0, (%edi)
    addl $8, %esp               # pop lenA, B
    movl %edx, %eax
    movl $0xFFFF0000, %edx
    ret
.Lsc_fail:
    addl $12, %esp
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret

# .Lint_fmt: esi = i_lo, edi = i_hi (unboxed i64, s32 range only)
# -> eax = ptr to decimal digits (int_scratch), ebx = len; eax=0 on range fail.
# clobbers eax,ebx,ecx,edx,edi,esi
.Lint_fmt:
    testl %edi, %edi
    jnz .Llf_negchk
    cmpl $0x80000000, %esi
    jae .Llf_fail
    jmp .Llf_go
.Llf_negchk:
    cmpl $0xFFFFFFFF, %edi
    jne .Llf_fail
    cmpl $0x80000000, %esi
    jb .Llf_fail
.Llf_go:
    movl %esi, %eax
    xorl %ebx, %ebx            # neg flag
    testl %eax, %eax
    jns .Llf_pos
    movl $1, %ebx
    cmpl $0x80000000, %eax
    je .Llf_min
    negl %eax
.Llf_pos:
    movl $int_scratch, %edi
    addl $15, %edi             # write cursor (backward)
.Llf_dig:
    xorl %edx, %edx
    movl $10, %ecx
    divl %ecx                  # eax=quot edx=rem (unsigned; eax<2^31 or 2^31)
    addl $0x30, %edx
    movb %dl, (%edi)
    decl %edi
    testl %eax, %eax
    jnz .Llf_dig
    testl %ebx, %ebx
    jz .Llf_fin
    movb $'-', (%edi)
    decl %edi
.Llf_fin:
    movl $int_scratch, %eax
    addl $15, %eax
    subl %edi, %eax            # len
    movl %eax, %ebx
    incl %edi
    movl %edi, %eax
    ret
.Llf_min:                     # INT64_MIN-of-s32 edge: -2147483648
    movl $0x80000000, %eax
    movl $1, %ebx
    jmp .Llf_dig
.Llf_fail:
    xorl %eax, %eax
    ret

# .Labs: eax = aval lo, edx = aval hi -> eax:edx abs(int) or A_NONE.
# clobbers ecx,esi,edi
.Labs:
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lax_none
    testl $1, %eax
    jz .Lax_none
    movl %eax, %esi
    movl %edx, %edi
    call .Lunbox               # esi=i_lo edi=i_hi
    testl %edi, %edi
    jns .Lax_pos
    notl %esi
    notl %edi
    addl $1, %esi
    adcl $0, %edi
.Lax_pos:
    call .Lrebox
    ret
.Lax_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret

# ---- collections ---------------------------------------------------------
# A_NONE = 0xFFFF000000000002

# a_range_new(lo,hi,step) : three tagged-int avals -> ARange or A_NONE.
    .globl a_range_new
a_range_new:
    pushl %ebp
    movl %esp, %ebp
    # tagged-int checks: argN aval lo@8+8k hi@12+8k
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lrn_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lrn_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lrn_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lrn_none
    movl 28(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lrn_none
    movl 24(%ebp), %eax
    testl $1, %eax
    jz .Lrn_none
    # unbox step,hi,lo (i64 each in esi:edi); push pairs -> [esp]=lo,hi,step
    movl 24(%ebp), %esi
    movl 28(%ebp), %edi
    call .Lunbox
    pushl %edi
    pushl %esi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    pushl %edi
    pushl %esi
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %edi
    pushl %esi
    # step == 0? at [esp+16],[esp+20]
    movl 16(%esp), %eax
    orl 20(%esp), %eax
    jz .Lrn_pop_none
    movl $48, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lrn_pop_none
    movl %eax, %edi             # obj
    movl $6, (%edi)             # T_RANGE
    movl $3, 4(%edi)            # len = 3 (matches C)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl (%esp), %eax
    movl %eax, 16(%edi)         # lo
    movl 4(%esp), %eax
    movl %eax, 20(%edi)
    movl 8(%esp), %eax
    movl %eax, 24(%edi)         # hi
    movl 12(%esp), %eax
    movl %eax, 28(%edi)
    movl 16(%esp), %eax
    movl %eax, 32(%edi)         # step
    movl 20(%esp), %eax
    movl %eax, 36(%edi)
    addl $24, %esp
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lrn_pop_none:
    addl $24, %esp
.Lrn_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# .Larr_new_impl: esi = elem type (2 list / 3 tuple); ebp frame set;
# args n u64 @8, items aval* @16. -> aval. Shared by a_list_new/a_tuple_new.
.Larr_new_impl:
    movl 12(%ebp), %eax         # n hi32
    testl %eax, %eax
    jnz .Lan_none
    movl 8(%ebp), %ebx          # n
    cmpl $4096, %ebx            # heap is 60K; cap literal sizes
    ja .Lan_none
    pushl %ebx                  # save n
    movl %ebx, %eax
    shll $3, %eax
    addl $40, %eax              # 40 hdr (items@16,iitems@24,flags@32) + n*8 inline
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lan_fail
    movl %eax, %edi             # obj
    movl %esi, (%edi)           # type
    movl (%esp), %eax           # n (pushed ebx; 4(%esp) is saved ebp)
    movl %eax, 4(%edi)          # len
    movl %eax, 8(%edi)          # cap = n
    movl $0, 12(%edi)
    movl %edi, %eax
    addl $40, %eax
    movl %eax, 16(%edi)         # items = obj+40
    movl $0, 24(%edi)           # iitems = NULL (not packed)
    movl $0, 32(%edi)           # flags = 0
    movl 16(%ebp), %ecx         # src aval array (flat ptr)
    movl (%esp), %edx
    shll $3, %edx               # nbytes
    movl %edi, %ebx
    addl $40, %ebx              # dst cursor
.Lan_cp:
    testl %edx, %edx
    jz .Lan_done
    movl (%ecx), %eax
    movl %eax, (%ebx)
    movl 4(%ecx), %eax
    movl %eax, 4(%ebx)
    addl $8, %ecx
    addl $8, %ebx
    subl $8, %edx
    jmp .Lan_cp
.Lan_done:
    addl $4, %esp
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lan_fail:
    addl $4, %esp
.Lan_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_list_new
a_list_new:
    pushl %ebp
    movl %esp, %ebp
    movl $2, %esi
    jmp .Larr_new_impl

    .globl a_tuple_new
a_tuple_new:
    pushl %ebp
    movl %esp, %ebp
    movl $3, %esi
    jmp .Larr_new_impl

# a_set_new() : empty T_SET (same AArr layout). No args.
    .globl a_set_new
a_set_new:
    pushl %ebp
    movl %esp, %ebp
    movl $48, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lsetn_none
    movl %eax, %edi
    movl $5, (%edi)             # T_SET
    movl $0, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl $0, 24(%edi)           # iitems
    movl $0, 32(%edi)           # flags
    movl %edi, %eax
    addl $40, %eax
    movl %eax, 16(%edi)         # items (unused while cap=0)
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lsetn_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_dict_new() : empty ADict, pairs_ptr = 0 until first set.
    .globl a_dict_new
a_dict_new:
    pushl %ebp
    movl %esp, %ebp
    movl $24, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Ldn_none
    movl %eax, %edi
    movl $4, (%edi)             # T_DICT
    movl $0, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl $0, 16(%edi)           # pairs = NULL
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Ldn_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# .Ldict_put: esi = dict obj; key aval ptr edi (flat); val aval on stack?
# We instead pass val via registers set by caller before jmp: too messy —
# a_dict_set inlines find+put. .Ldict_growappend helper:
# esi = dict, edi = flat ptr to key aval, and val aval copied from
# caller-supplied stack args is handled inline in a_dict_set.

# a_dict_set(d@8, k@16, v@24) -> A_NONE
    .globl a_dict_set
a_dict_set:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lds_none
    cmpl $4, (%esi)
    jne .Lds_none
    # key flat ptr = ebp + 0x90000 + 16
    movl %ebp, %edi
    addl $0x90010, %edi
    call .Ldict_find          # esi preserved, eax = idx/-1
    testl %eax, %eax
    jns .Lds_replace
    # append: ensure cap
    call .Ldict_ensure
    testl %eax, %eax
    jz .Lds_none
    movl 4(%esi), %ecx        # len = pair index
    movl 16(%esi), %edx       # pairs
    shll $4, %ecx
    addl %ecx, %edx           # &pair
    movl 16(%ebp), %eax       # k lo
    movl %eax, (%edx)
    movl 20(%ebp), %eax       # k hi
    movl %eax, 4(%edx)
    movl 24(%ebp), %eax       # v lo
    movl %eax, 8(%edx)
    movl 28(%ebp), %eax       # v hi
    movl %eax, 12(%edx)
    incl 4(%esi)
    jmp .Lds_none
.Lds_replace:
    movl 16(%esi), %ecx       # pairs
    shll $4, %eax
    addl %eax, %ecx           # &pair[idx]
    movl 24(%ebp), %edx       # v lo
    movl %edx, 8(%ecx)
    movl 28(%ebp), %edx       # v hi
    movl %edx, 12(%ecx)
    jmp .Lds_none
.Lds_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_dict_get(d@8, k@16, dflt@24) -> aval
    .globl a_dict_get
a_dict_get:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Ldg_dflt
    cmpl $4, (%esi)
    jne .Ldg_dflt
    movl %ebp, %edi
    addl $0x90010, %edi       # &k
    call .Ldict_find
    testl %eax, %eax
    js .Ldg_dflt
    movl 16(%esi), %ecx
    shll $4, %eax
    addl %eax, %ecx
    movl 8(%ecx), %eax        # v lo
    movl 12(%ecx), %edx       # v hi
    popl %ebp
    ret
.Ldg_dflt:
    movl 24(%ebp), %eax
    movl 28(%ebp), %edx
    popl %ebp
    ret

# a_list_push(list@8, v@16) -> aval (returns list, like C)
    .globl a_list_push
a_list_push:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Llp_out
    cmpl $2, (%esi)
    jne .Llp_out
    call .Larr_ensure
    testl %eax, %eax
    jz .Llp_out
    movl 4(%esi), %ecx        # len
    movl 16(%esi), %edx       # items
    shll $3, %ecx
    addl %ecx, %edx
    movl 16(%ebp), %eax
    movl %eax, (%edx)
    movl 20(%ebp), %eax
    movl %eax, 4(%edx)
    incl 4(%esi)
.Llp_out:
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    popl %ebp
    ret

# a_set_add(s@8, v@16) -> A_NONE ; dedup via .Leqkey
    .globl a_set_add
a_set_add:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lsa_none
    cmpl $5, (%esi)
    jne .Lsa_none
    pushl %esi                  # [ebp-4] set obj
    # v flat ptr = ebp + 0x90000 + 16
    movl %ebp, %edi
    addl $0x90010, %edi
    pushl $0                    # [ebp-8] i
.Lsa_scan:
    movl -8(%ebp), %eax         # i
    movl -4(%ebp), %edx         # set
    movl 4(%edx), %ecx          # len
    cmpl %ecx, %eax
    jae .Lsa_add
    movl 16(%edx), %ecx         # items
    movl %eax, %edx
    shll $3, %edx
    addl %edx, %ecx             # &items[i]
    pushl %edi                  # save v ptr (.Leqkey preserves edi anyway)
    movl %ecx, %esi
    call .Leqkey
    popl %edi
    testl %eax, %eax
    jnz .Lsa_none_pop           # already present
    incl -8(%ebp)
    jmp .Lsa_scan
.Lsa_add:
    movl -4(%ebp), %esi         # set obj
    call .Larr_ensure
    testl %eax, %eax
    jz .Lsa_none_pop
    movl -4(%ebp), %esi
    movl 4(%esi), %ecx          # len
    movl 16(%esi), %edx         # items
    shll $3, %ecx
    addl %ecx, %edx
    movl 16(%ebp), %eax         # v lo
    movl %eax, (%edx)
    movl 20(%ebp), %eax         # v hi
    movl %eax, 4(%edx)
    incl 4(%esi)
.Lsa_none_pop:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    movl %ebp, %esp
    popl %ebp
    ret
.Lsa_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_str_new(char *p @8(%ebp) [4B], uint64 len @12(%ebp) [8B]) -> aval
# Bump-allocates AStr {u32 type=1, u32 len, u64 cap, char data[]}: 16-byte
# header + len bytes + NUL, whole object 16-aligned so the returned aval
# (A_TAG | ptr) satisfies the pointer payload rules. Exhaustion -> A_NONE.
    .globl a_str_new
a_str_new:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %esi         # src ptr (flat physical, DS=0)
    movl 12(%ebp), %ebx        # len lo32
    movl 16(%ebp), %ecx        # len hi32
    testl %ecx, %ecx
    jnz .Lsn_fail              # len >= 4G unsupported
    movl %ebx, %eax
    addl $32, %eax            # 16 hdr + len + 1 NUL + pad-to-16 (round UP)
    andl $0xFFFFFFF0, %eax
    movl heap_cur, %edi
    movl heap_end, %ecx
    movl %edi, %edx
    addl %eax, %edx
    cmpl %ecx, %edx
    ja .Lsn_fail
    movl %edx, heap_cur
    movl $1, (%edi)            # T_STR
    movl %ebx, 4(%edi)         # len
    movl %ebx, %eax
    incl %eax
    movl %eax, 8(%edi)         # cap = len + 1
    movl %edi, %ecx
    addl $16, %ecx             # dst = data
.Lsn_copy:
    testl %ebx, %ebx
    jz .Lsn_nul
    movb (%esi), %al
    movb %al, (%ecx)
    incl %esi
    incl %ecx
    decl %ebx
    jmp .Lsn_copy
.Lsn_nul:
    movb $0, (%ecx)
    movl %edi, %eax            # aval lo = obj ptr
    movl $0xFFFF0000, %edx     # aval hi = A_TAG
    popl %ebp
    ret
.Lsn_fail:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
# a_char_new(char *p @8(%ebp) [4B], uint64 len @12(%ebp) [8B]) -> aval
# Same layout as a_str_new but type = T_CHAR (12).
    .globl a_char_new
a_char_new:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %esi
    movl 12(%ebp), %ebx
    movl 16(%ebp), %ecx
    testl %ecx, %ecx
    jnz .Lcn_fail
    movl %ebx, %eax
    addl $32, %eax
    andl $0xFFFFFFF0, %eax
    movl heap_cur, %edi
    movl heap_end, %ecx
    movl %edi, %edx
    addl %eax, %edx
    cmpl %ecx, %edx
    ja .Lcn_fail
    movl %edx, heap_cur
    movl $12, (%edi)            # T_CHAR
    movl %ebx, 4(%edi)
    movl %ebx, %eax
    incl %eax
    movl %eax, 8(%edi)
    movl %edi, %ecx
    addl $16, %ecx
.Lcn_copy:
    testl %ebx, %ebx
    jz .Lcn_nul
    movb (%esi), %al
    movb %al, (%ecx)
    incl %esi
    incl %ecx
    decl %ebx
    jmp .Lcn_copy
.Lcn_nul:
    movb $0, (%ecx)
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lcn_fail:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_len(v@8) -> aval int or A_NONE
    .globl a_len
a_len:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lln_none
    movl (%esi), %ecx           # type
    cmpl $6, %ecx
    je .Lln_range
    cmpl $1, %ecx
    je .Lln_utf8
    cmpl $12, %ecx
    je .Lln_utf8
    cmpl $2, %ecx
    je .Lln_fast
    cmpl $3, %ecx
    je .Lln_fast
    cmpl $4, %ecx
    je .Lln_fast
    cmpl $5, %ecx
    je .Lln_fast
    jmp .Lln_none
.Lln_fast:                      # len@4 = element/byte count
    movl 4(%esi), %eax
    call .Lmksmall
    popl %ebp
    ret
.Lln_utf8:                      # count UTF-8 lead bytes (C u8_count)
    movl 4(%esi), %ebx          # nbytes
    movl %esi, %edi
    addl $16, %edi              # data
    xorl %ecx, %ecx             # count
.Lln_u8loop:
    testl %ebx, %ebx
    jz .Lln_u8done
    movzbl (%edi), %eax
    movl %eax, %edx
    andl $0xC0, %edx
    cmpl $0x80, %edx
    je .Lln_u8skip
    incl %ecx
.Lln_u8skip:
    incl %edi
    decl %ebx
    jmp .Lln_u8loop
.Lln_u8done:
    movl %ecx, %eax
    call .Lmksmall
    popl %ebp
    ret
.Lln_range:                     # count = step>0 ? (hi>lo ? (hi-lo-1)/step+1 : 0)
    #                        : (lo>hi ? (lo-hi-1)/(-step)+1 : 0)
    # s32 fast path only; falls back to A_NONE for huge bounds.
    movl 32(%esi), %eax          # step lo
    movl 36(%esi), %ecx          # step hi
    orl %ecx, %eax
    jz .Lln_r0
    # |step| -> ebx (s32)
    testl %ecx, %ecx
    jns .Lln_rsp
    cmpl $0xFFFFFFFF, %ecx
    jne .Lln_rnone
    negl %eax                    # -step_lo (step<0, hi=-1)
    movl %eax, %ebx
    jmp .Lln_rstep
.Lln_rsp:
    testl %ecx, %ecx
    jnz .Lln_rnone
    cmpl $0x80000000, %eax
    jae .Lln_rnone
    movl %eax, %ebx
    testl %ebx, %ebx
    jz .Lln_rnone
.Lln_rstep:
    # D = step>0 ? hi-lo : lo-hi  (i64 in edx:eax)
    movl 36(%esi), %ecx
    testl %ecx, %ecx
    jns .Lln_rpd
    movl 16(%esi), %eax          # lo lo
    movl 20(%esi), %edx          # lo hi
    subl 24(%esi), %eax          # - hi lo
    sbbl 28(%esi), %edx
    jmp .Lln_rdd
.Lln_rpd:
    movl 24(%esi), %eax          # hi lo
    movl 28(%esi), %edx          # hi hi
    subl 16(%esi), %eax          # - lo lo
    sbbl 20(%esi), %edx
.Lln_rdd:
    testl %edx, %edx
    jnz .Lln_rchkneg
    testl %eax, %eax
    jz .Lln_r0
    cmpl $0x80000001, %eax
    ja .Lln_rnone
    decl %eax                    # D-1
    xorl %edx, %edx
    divl %ebx                    # (D-1)/|step|
    incl %eax
    jmp .Lln_smalldone
.Lln_rchkneg:
    cmpl $0xFFFFFFFF, %edx
    je .Lln_r0                   # D < 0 -> empty
    jmp .Lln_rnone               # D >= 4G -> too big
.Lln_r0:
    xorl %eax, %eax
    call .Lmksmall
    popl %ebp
    ret
.Lln_smalldone:
    call .Lmksmall
    popl %ebp
    ret
.Lln_rnone:
    jmp .Lln_none
.Lln_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_getitem(obj@8, idx@16) -> aval
    .globl a_getitem
a_getitem:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lgi_none
    movl %esi, %ebx             # obj
    # T_DICT keys are arbitrary avals (not positional ints): dispatch
    # before the tagged-int index check below.
    cmpl $4, (%ebx)
    je .Lgi_dict
    # idx tagged int?
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lgi_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lgi_none
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox               # esi=i_lo edi=i_hi
    movl 4(%ebx), %ecx          # len
    cmpl $0xFFFFFFFF, %edi
    jne .Lgi_chkpos
    addl %ecx, %esi             # negative: idx = lo32 + len (wraps)
    cmpl %ecx, %esi
    jae .Lgi_none
    jmp .Lgi_idx
.Lgi_chkpos:
    testl %edi, %edi
    jnz .Lgi_none
    cmpl %ecx, %esi
    jae .Lgi_none
.Lgi_idx:
    movl (%ebx), %ecx           # type
    cmpl $1, %ecx
    je .Lgi_str
    cmpl $12, %ecx
    je .Lgi_none                # char not subscriptable
    cmpl $2, %ecx
    je .Lgi_arr
    cmpl $3, %ecx
    je .Lgi_arr
    cmpl $4, %ecx
    je .Lgi_dict
    jmp .Lgi_none
.Lgi_arr:
    movl 16(%ebx), %eax         # items
    movl %esi, %edx
    shll $3, %edx
    addl %edx, %eax             # &item aval
    movl (%eax), %ecx
    movl 4(%eax), %edx
    movl %ecx, %eax
    jmp .Lgi_out
.Lgi_dict:
    pushl %ebx
    movl %ebx, %esi
    movl %ebp, %edi
    addl $0x90010, %edi         # &idx aval (arg2 @16)
    call .Ldict_find
    popl %ebx
    testl %eax, %eax
    js .Lgi_none
    movl 16(%ebx), %ecx         # pairs
    shll $4, %eax
    addl %eax, %ecx
    movl 8(%ecx), %eax          # v lo
    movl 12(%ecx), %edx         # v hi
    jmp .Lgi_out
.Lgi_str:
    movl %ebx, %eax
    addl $16, %eax
    addl %esi, %eax             # data + idx
    pushl $0
    pushl $1
    pushl %eax
    call a_str_new
    addl $12, %esp
    jmp .Lgi_out
.Lgi_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
.Lgi_out:
    popl %ebp
    ret

# a_setitem(obj@8, k@16, v@24) -> aval (val on success, A_NONE else)
    .globl a_setitem
a_setitem:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lsi_none
    movl (%esi), %ecx
    cmpl $4, %ecx
    je .Lsi_dict
    cmpl $2, %ecx
    jne .Lsi_none
    # list: idx tagged int @16
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lsi_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lsi_none
    movl %esi, %ebx
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    movl 4(%ebx), %ecx
    cmpl $0xFFFFFFFF, %edi
    jne .Lsi_chkpos
    addl %ecx, %esi
    cmpl %ecx, %esi
    jae .Lsi_none
    jmp .Lsi_idx
.Lsi_chkpos:
    testl %edi, %edi
    jnz .Lsi_none
    cmpl %ecx, %esi
    jae .Lsi_none
.Lsi_idx:
    movl 16(%ebx), %edx         # items
    shll $3, %esi
    addl %esi, %edx
    movl 24(%ebp), %eax         # v lo
    movl %eax, (%edx)
    movl 28(%ebp), %eax         # v hi
    movl %eax, 4(%edx)
    movl 24(%ebp), %eax
    movl 28(%ebp), %edx
    jmp .Lsi_out
.Lsi_dict:
    pushl %esi                  # [ebp-4] dict (.Leqkey/.Ldict_ensure clobber ebx)
    movl %ebp, %edi
    addl $0x90010, %edi         # &k
    call .Ldict_find
    testl %eax, %eax
    jns .Lsi_drepl
    movl -4(%ebp), %esi         # dict for ensure (esi preserved anyway)
    call .Ldict_ensure
    testl %eax, %eax
    jz .Lsi_dnone
    movl -4(%ebp), %ebx         # dict
    movl 4(%ebx), %ecx
    movl 16(%ebx), %edx
    shll $4, %ecx
    addl %ecx, %edx
    movl 16(%ebp), %eax         # k lo
    movl %eax, (%edx)
    movl 20(%ebp), %eax
    movl %eax, 4(%edx)
    movl 24(%ebp), %eax         # v lo
    movl %eax, 8(%edx)
    movl 28(%ebp), %eax
    movl %eax, 12(%edx)
    incl 4(%ebx)
    movl 24(%ebp), %eax
    movl 28(%ebp), %edx
    addl $4, %esp
    jmp .Lsi_out
.Lsi_drepl:
    movl -4(%ebp), %ebx         # dict
    movl 16(%ebx), %ecx
    shll $4, %eax
    addl %eax, %ecx
    movl 24(%ebp), %eax
    movl %eax, 8(%ecx)
    movl 28(%ebp), %eax
    movl %eax, 12(%ecx)
    movl 24(%ebp), %eax
    movl 28(%ebp), %edx
    addl $4, %esp
.Lsi_out:
    popl %ebp
    ret
.Lsi_dnone:
    addl $4, %esp
.Lsi_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_contains(cont@8, item@16) -> A_TRUE/A_FALSE aval
    .globl a_contains
a_contains:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lct_false
    movl (%esi), %ecx
    cmpl $4, %ecx
    je .Lct_dict
    cmpl $6, %ecx
    je .Lct_range
    cmpl $1, %ecx
    je .Lct_str
    cmpl $2, %ecx
    je .Lct_scan
    cmpl $3, %ecx
    je .Lct_scan
    cmpl $5, %ecx
    je .Lct_scan
    jmp .Lct_false
.Lct_dict:
    movl %esi, %ebx
    movl %ebx, %esi
    movl %ebp, %edi
    addl $0x90010, %edi         # &item
    call .Ldict_find
    testl %eax, %eax
    js .Lct_false
    jmp .Lct_true
.Lct_scan:
    pushl %esi                  # [ebp-4] cont
    pushl $0                    # [ebp-8] i
.Lct_sc_loop:
    movl -8(%ebp), %eax
    movl -4(%ebp), %edx
    movl 4(%edx), %ecx          # len
    cmpl %ecx, %eax
    jae .Lct_sc_false
    movl 16(%edx), %ecx         # items
    movl %eax, %edx
    shll $3, %edx
    addl %edx, %ecx             # &items[i]
    pushl %edi
    movl %ecx, %esi
    movl %ebp, %edi
    addl $0x90010, %edi         # &item
    call .Leqkey
    popl %edi
    testl %eax, %eax
    jnz .Lct_sc_true
    incl -8(%ebp)
    jmp .Lct_sc_loop
.Lct_sc_true:
    movl $6, %eax
    movl $0xFFFF0000, %edx
    jmp .Lct_out
.Lct_sc_false:
    movl %ebp, %esp
    jmp .Lct_pop
.Lct_str:
    # esi = hay obj; item must be T_STR
    movl %esi, %ebx             # ebx = hay
    movl 16(%ebp), %eax
    movl 20(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lct_false
    cmpl $1, (%esi)
    jne .Lct_false
    # esi = needle obj, ebx = hay obj
    movl 4(%ebx), %ecx          # hay len
    movl 4(%esi), %edx          # needle len
    cmpl %ecx, %edx
    ja .Lct_false
    testl %edx, %edx
    jz .Lct_true                # empty needle -> true
    subl %edx, %ecx             # max_start = haylen - needlelen
    # stack slots: [esp]=pos, [esp+4]=cnt, [esp+8]=haydata, [esp+12]=needledata
    movl %esi, %eax
    addl $16, %eax
    pushl %eax                  # needle data
    movl %ebx, %eax
    addl $16, %eax
    pushl %eax                  # hay data
    pushl %edx                  # cnt = needle len
    pushl $0                    # pos
.Lct_spos:
    movl (%esp), %eax           # pos
    cmpl %ecx, %eax
    ja .Lct_sfalse
    movl 4(%esp), %edx          # cnt
    movl 8(%esp), %edi          # hay data
    addl %eax, %edi             # hcur
    movl 12(%esp), %esi         # ncur
.Lct_scmp:
    testl %edx, %edx
    jz .Lct_smatch
    movb (%edi), %al
    cmpb %al, (%esi)
    jne .Lct_snext
    incl %edi
    incl %esi
    decl %edx
    jmp .Lct_scmp
.Lct_snext:
    incl (%esp)                 # pos++
    jmp .Lct_spos
.Lct_smatch:
    addl $16, %esp
    jmp .Lct_true
.Lct_sfalse:
    addl $16, %esp
    jmp .Lct_false
.Lct_range:
    # esi = ARange obj; item must be tagged int -> s32 fast path membership
    movl %esi, %ebx
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lct_false
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lct_false
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox               # esi=v_lo edi=v_hi
    # v must fit s32
    testl %edi, %edi
    jnz .Lct_vneg
    cmpl $0x80000000, %esi
    jae .Lct_false
    jmp .Lct_vok
.Lct_vneg:
    cmpl $0xFFFFFFFF, %edi
    jne .Lct_false
    cmpl $0x80000000, %esi
    jb .Lct_false
.Lct_vok:                       # esi = s32 v
    # lo -> eax (s32)
    movl 20(%ebx), %eax
    testl %eax, %eax
    jnz .Lct_loneg
    cmpl $0x80000000, 16(%ebx)
    jae .Lct_false
    movl 16(%ebx), %eax
    jmp .Lct_hik
.Lct_loneg:
    cmpl $0xFFFFFFFF, %eax
    jne .Lct_false
    cmpl $0x80000000, 16(%ebx)
    jb .Lct_false
    movl 16(%ebx), %eax
.Lct_hik:                       # hi -> ecx (s32)
    movl 28(%ebx), %edx
    testl %edx, %edx
    jnz .Lct_hineg
    cmpl $0x80000000, 24(%ebx)
    jae .Lct_false
    movl 24(%ebx), %ecx
    jmp .Lct_stk
.Lct_hineg:
    cmpl $0xFFFFFFFF, %edx
    jne .Lct_false
    cmpl $0x80000000, 24(%ebx)
    jb .Lct_false
    movl 24(%ebx), %ecx
.Lct_stk:                       # step -> edx (s32, nonzero)
    movl 36(%ebx), %edi
    testl %edi, %edi
    jnz .Lct_sneg
    cmpl $0x80000000, 32(%ebx)
    jae .Lct_false
    movl 32(%ebx), %edx
    testl %edx, %edx
    jz .Lct_false
    jmp .Lct_calc
.Lct_sneg:
    cmpl $0xFFFFFFFF, %edi
    jne .Lct_false
    cmpl $0x80000000, 32(%ebx)
    jb .Lct_false
    movl 32(%ebx), %edx
    testl %edx, %edx
    jz .Lct_false
.Lct_calc:                      # eax=lo esi=v ecx=hi edx=step (all s32)
    testl %edx, %edx
    jns .Lct_cpos
    # step<0: in (hi, lo]  -> v<=lo && v>hi
    cmpl %eax, %esi
    jg .Lct_false
    cmpl %ecx, %esi
    jle .Lct_false
    jmp .Lct_cmod
.Lct_cpos:
    # step>0: in [lo, hi) -> v>=lo && v<hi
    cmpl %eax, %esi
    jl .Lct_false
    cmpl %ecx, %esi
    jge .Lct_false
.Lct_cmod:
    cmpl $0xFFFFFFFF, %edx
    je .Lct_true               # step == -1 -> always on grid
    pushl %edx                 # step
    movl %esi, %ecx
    subl %eax, %ecx            # diff = v - lo (s32 wrap; subset)
    movl %ecx, %eax
    cdq
    popl %ecx                  # step
    idivl %ecx
    testl %edx, %edx
    jnz .Lct_false
    jmp .Lct_true
.Lct_true:
    movl $6, %eax
    movl $0xFFFF0000, %edx
    jmp .Lct_pop
.Lct_false:
    movl $4, %eax
    movl $0xFFFF0000, %edx
.Lct_pop:
    movl %ebp, %esp
    popl %ebp
    ret
.Lct_out:
    movl %ebp, %esp
    popl %ebp
    ret

# a_iter_new(v@8) -> iter aval or A_NONE
    .globl a_iter_new
a_iter_new:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lin_new_none
    movl (%esi), %ecx
    cmpl $1, %ecx               # T_STR
    je .Lin_new_ok
    cmpl $2, %ecx
    je .Lin_new_ok
    cmpl $3, %ecx
    je .Lin_new_ok
    cmpl $4, %ecx
    je .Lin_new_ok
    cmpl $5, %ecx
    je .Lin_new_ok
    cmpl $6, %ecx
    je .Lin_new_ok
    jmp .Lin_new_none
.Lin_new_ok:
    movl $64, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lin_new_none
    movl %eax, %edi             # iter
    movl $7, (%edi)             # T_ITER
    movl $0, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl 8(%ebp), %eax          # src aval lo
    movl %eax, 16(%edi)
    movl 12(%ebp), %eax         # src aval hi
    movl %eax, 20(%edi)
    movl $0, 24(%edi)           # idx
    movl $0, 28(%edi)
    movl $0, 32(%edi)           # cur
    movl $0, 36(%edi)
    movl $0, 40(%edi)           # end
    movl $0, 44(%edi)
    movl $0, 48(%edi)           # step
    movl $0, 52(%edi)
    movl (%esi), %ecx           # reload src type (.Lalloc clobbered ecx)
    cmpl $6, %ecx
    jne .Lin_new_done
    # range: cur=lo@16, end=hi@24, step=step@32 of src obj (esi)
    movl 16(%esi), %eax
    movl %eax, 32(%edi)
    movl 20(%esi), %eax
    movl %eax, 36(%edi)
    movl 24(%esi), %eax
    movl %eax, 40(%edi)
    movl 28(%esi), %eax
    movl %eax, 44(%edi)
    movl 32(%esi), %eax
    movl %eax, 48(%edi)
    movl 36(%esi), %eax
    movl %eax, 52(%edi)
.Lin_new_done:
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lin_new_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_iter_next(it@8, out@16 ptr) -> eax=1/0, edx=0
    .globl a_iter_next
a_iter_next:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lxn_zero
    cmpl $7, (%esi)
    jne .Lxn_zero
    movl %esi, %ebx             # iter
    pushl %ebx                  # [ebp-4] iter (calls clobber ebx)
    pushl 16(%ebp)              # [ebp-8] out ptr
    movl 16(%ebx), %eax         # src aval lo
    movl 20(%ebx), %edx         # src aval hi
    call .Lobjptr
    testl %esi, %esi
    jz .Lxn_zero_p
    movl (%esi), %ecx           # src type
    cmpl $6, %ecx
    je .Lxn_range
    # seq: len@4 (bytes for str)
    movl 4(%esi), %eax
    movl 24(%ebx), %edx         # idx
    cmpl %eax, %edx
    jae .Lxn_zero_p
    cmpl $4, %ecx
    je .Lxn_ditem
    cmpl $1, %ecx
    je .Lxn_sitem
    # arr item
    movl 16(%esi), %eax
    movl 24(%ebx), %edx
    shll $3, %edx
    addl %edx, %eax
    movl (%eax), %edx
    movl 4(%eax), %ecx
    movl -8(%ebp), %eax         # out ptr
    movl %edx, (%eax)
    movl %ecx, 4(%eax)
    incl 24(%ebx)
    jmp .Lxn_yes
.Lxn_ditem:
    movl 16(%esi), %eax         # pairs
    movl 24(%ebx), %edx
    shll $4, %edx
    addl %edx, %eax
    movl (%eax), %edx
    movl 4(%eax), %ecx
    movl -8(%ebp), %eax
    movl %edx, (%eax)
    movl %ecx, 4(%eax)
    incl 24(%ebx)
    jmp .Lxn_yes
.Lxn_sitem:
    movl 24(%ebx), %edx
    movl %esi, %eax
    addl $16, %eax
    addl %edx, %eax             # data + idx
    pushl $0
    pushl $1
    pushl %eax
    call a_str_new
    addl $12, %esp
    movl -8(%ebp), %ecx         # out ptr
    movl %eax, (%ecx)
    movl %edx, 4(%ecx)
    movl -4(%ebp), %ebx         # iter (ebx clobbered by a_str_new)
    incl 24(%ebx)
    jmp .Lxn_yes
.Lxn_range:
    movl 52(%ebx), %ecx         # step hi
    testl %ecx, %ecx
    js .Lxn_rneg
    movl 36(%ebx), %eax         # cur hi
    movl 44(%ebx), %edx         # end hi
    cmpl %edx, %eax
    jl .Lxn_remit
    jg .Lxn_zero_p
    movl 32(%ebx), %eax         # cur lo
    cmpl 40(%ebx), %eax         # end lo
    jb .Lxn_remit
    jmp .Lxn_zero_p
.Lxn_rneg:
    movl 36(%ebx), %eax
    movl 44(%ebx), %edx
    cmpl %edx, %eax
    jg .Lxn_remit
    jl .Lxn_zero_p
    movl 32(%ebx), %eax
    cmpl 40(%ebx), %eax
    ja .Lxn_remit
    jmp .Lxn_zero_p
.Lxn_remit:
    movl 32(%ebx), %esi
    movl 36(%ebx), %edi
    call .Lrebox
    movl -8(%ebp), %ecx
    movl %eax, (%ecx)
    movl %edx, 4(%ecx)
    movl 32(%ebx), %esi
    movl 36(%ebx), %edi
    movl 48(%ebx), %eax         # step lo
    movl 52(%ebx), %ecx         # step hi
    addl %eax, %esi
    adcl %ecx, %edi
    movl %esi, 32(%ebx)
    movl %edi, 36(%ebx)
    jmp .Lxn_yes
.Lxn_yes:
    movl $1, %eax
    xorl %edx, %edx
    jmp .Lxn_out
.Lxn_zero_p:
    movl $0, %eax
    xorl %edx, %edx
    jmp .Lxn_out
.Lxn_zero:
    xorl %eax, %eax
    xorl %edx, %edx
.Lxn_out:
    movl %ebp, %esp            # drop the iter/out pushes (and any str path)
    popl %ebp
    ret


# a_in_range(v@8, lo@16, hi@24) : three tagged ints -> A_TRUE/A_FALSE
# (pattern `case lo..hi` lowering; hi exclusive, step 1)
    .globl a_in_range
a_in_range:
    pushl %ebp
    movl %esp, %ebp
    # tagged-int checks for v,lo,hi
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lir_false
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lir_false
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lir_false
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lir_false
    movl 28(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lir_false
    movl 24(%ebp), %eax
    testl $1, %eax
    jz .Lir_false
    # unbox v,hi (push pairs), keep lo in esi:edi
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox               # v
    pushl %edi
    pushl %esi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox               # hi
    pushl %edi
    pushl %esi
    movl 24(%ebp), %esi
    movl 28(%ebp), %edi
    call .Lunbox               # lo -> esi:edi
    # stack: [esp]=hi_lo,[esp+4]=hi_hi,[esp+8]=v_lo,[esp+12]=v_hi
    # v >= lo ?
    movl 12(%esp), %eax        # v_hi
    cmpl %edi, %eax
    jl .Lir_false
    jg .Lir_vge
    movl 8(%esp), %eax         # v_lo
    cmpl %esi, %eax
    jb .Lir_false
.Lir_vge:
    # v < hi ?
    movl 12(%esp), %eax
    cmpl 4(%esp), %eax
    jl .Lir_true
    jg .Lir_false
    movl 8(%esp), %eax
    cmpl (%esp), %eax
    jb .Lir_true
    jmp .Lir_false
.Lir_true:
    movl $6, %eax
    movl $0xFFFF0000, %edx
    addl $16, %esp
    popl %ebp
    ret
.Lir_false:
    movl $4, %eax
    movl $0xFFFF0000, %edx
    addl $16, %esp
    popl %ebp
    ret

# a_str(v@8) -> aval ; a_repr aliases (subset: no quoting of strings)
    .globl a_str
a_str:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    # tagged int?
    movl %edi, %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Las_obj
    testl $1, %esi
    jz .Las_obj
    call .Lunbox               # esi=i_lo edi=i_hi
    call .Lint_fmt             # eax=ptr or 0, ebx=len
    testl %eax, %eax
    jz .Las_none
    pushl $0
    pushl %ebx
    pushl %eax
    call a_str_new
    addl $12, %esp
    popl %ebp
    ret
.Las_obj:
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Las_small
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Las_self
    cmpl $12, %ecx
    je .Las_self
    jmp .Las_none
.Las_self:
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    popl %ebp
    ret
.Las_small:
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Las_none
    cmpl $2, %eax
    je .Las_w_none
    cmpl $4, %eax
    je .Las_w_false
    cmpl $6, %eax
    je .Las_w_true
    jmp .Las_none
.Las_w_none:
    movl $str_none, %esi
    movl $4, %ebx
    jmp .Las_wrap
.Las_w_false:
    movl $str_false, %esi
    movl $5, %ebx
    jmp .Las_wrap
.Las_w_true:
    movl $str_true, %esi
    movl $4, %ebx
    jmp .Las_wrap
.Las_wrap:
    pushl $0
    pushl %ebx
    pushl %esi
    call a_str_new
    addl $12, %esp
    popl %ebp
    ret
.Las_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_repr is defined below (buffer-based repr for collections).

# a_unwrap(v@8): AResult ok -> value; type10 !ok -> A_NONE; else v
    .globl a_unwrap
a_unwrap:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Luw_self
    cmpl $10, (%esi)
    jne .Luw_self
    cmpl $0, 16(%esi)
    je .Luw_none
    movl 24(%esi), %eax
    movl 28(%esi), %edx
    popl %ebp
    ret
.Luw_self:
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    popl %ebp
    ret
.Luw_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_some_new(v@8): AResult{ok=1,is_option=1,value=v}
    .globl a_some_new
a_some_new:
    pushl %ebp
    movl %esp, %ebp
    movl $32, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lsn2_none
    movl %eax, %edi
    movl $10, (%edi)            # T_RESULT
    movl $1, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl $1, 16(%edi)           # ok
    movl $1, 20(%edi)           # is_option
    movl 8(%ebp), %eax          # value lo
    movl %eax, 24(%edi)
    movl 12(%ebp), %eax         # value hi
    movl %eax, 28(%edi)
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lsn2_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_try_result(v@8): ok result -> value; else v
    .globl a_try_result
a_try_result:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Ltr_self
    cmpl $10, (%esi)
    jne .Ltr_self
    cmpl $0, 16(%esi)
    je .Ltr_self
    movl 24(%esi), %eax
    movl 28(%esi), %edx
    popl %ebp
    ret
.Ltr_self:
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    popl %ebp
    ret

# a_is_propagate(v@8) -> int32 1/0 (ret32 symbol)
    .globl a_is_propagate
a_is_propagate:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lip_zero
    cmpl $10, (%esi)
    jne .Lip_zero
    cmpl $0, 16(%esi)
    jne .Lip_zero
    movl $1, %eax
    xorl %edx, %edx
    popl %ebp
    ret
.Lip_zero:
    xorl %eax, %eax
    xorl %edx, %edx
    popl %ebp
    ret

# a_obj_is_instance -> 0 (no class support)
    .globl a_obj_is_instance
a_obj_is_instance:
    xorl %eax, %eax
    ret

# a_match_fail(v@8) -> A_NONE
    .globl a_match_fail
a_match_fail:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret

# ---- environments / closures ---------------------------------------------
# AEnv {type=9,len@4=n,slots aval[]@16}; AFnon {type=8,...,entry@16,env@20(8B),name@28,builtin@32}

# a_env_get(env@8, i@16 u64) -> aval
    .globl a_env_get
a_env_get:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Leg_none
    cmpl $9, (%esi)
    jne .Leg_none
    movl 20(%ebp), %eax         # i hi
    testl %eax, %eax
    jnz .Leg_none
    movl 16(%ebp), %ecx         # i
    cmpl 4(%esi), %ecx
    jae .Leg_none
    movl 16(%esi), %edx         # slots base
    shll $3, %ecx
    addl %ecx, %edx
    movl (%edx), %eax           # slot lo
    movl 4(%edx), %edx          # slot hi
    popl %ebp
    ret
.Leg_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_env_set(env@8, i@16 u64, v@24 aval) -> A_NONE
    .globl a_env_set
a_env_set:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Les_none
    cmpl $9, (%esi)
    jne .Les_none
    movl 20(%ebp), %eax
    testl %eax, %eax
    jnz .Les_none
    movl 16(%ebp), %ecx
    cmpl 4(%esi), %ecx
    jae .Les_none
    movl 16(%esi), %edx
    shll $3, %ecx
    addl %ecx, %edx
    movl 24(%ebp), %eax
    movl %eax, (%edx)
    movl 28(%ebp), %eax
    movl %eax, 4(%edx)
.Les_done:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Les_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret


# a_closure_env_new(n@8 u64) -> AEnv aval (slots init A_NONE)
    .globl a_closure_env_new
a_closure_env_new:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax         # n hi
    testl %eax, %eax
    jnz .Lcen_none
    movl 8(%ebp), %ebx          # n
    cmpl $2048, %ebx
    ja .Lcen_none
    movl %ebx, %eax
    shll $3, %eax
    addl $16, %eax
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lcen_none
    movl %eax, %edi
    movl $9, (%edi)             # T_ENV
    movl 8(%ebp), %eax          # n -> len
    movl %eax, 4(%edi)
    movl %edi, %eax
    addl $16, %eax
    movl %eax, 16(%edi)         # slots = obj+16
    # init slots to A_NONE
    movl 8(%ebp), %ecx          # n
    movl %edi, %edx
    addl $16, %edx              # slot cursor
.Lcen_init:
    testl %ecx, %ecx
    jz .Lcen_done
    movl $2, (%edx)
    movl $0xFFFF0000, 4(%edx)
    addl $8, %edx
    decl %ecx
    jmp .Lcen_init
.Lcen_done:
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lcen_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# shared AFnon ctor tail: edi=obj, entry@16, env aval, name, builtin id
# a_fnon_new(entry@8 u64, env@16 aval, name@24 ptr4) -> aval
    .globl a_fnon_new
a_fnon_new:
    pushl %ebp
    movl %esp, %ebp
    movl $48, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lfn_none
    movl %eax, %edi
    movl $8, (%edi)             # T_FNON
    movl $1, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl 8(%ebp), %eax          # entry lo32
    movl %eax, 16(%edi)
    movl 16(%ebp), %eax         # env lo
    movl %eax, 20(%edi)
    movl 20(%ebp), %eax         # env hi
    movl %eax, 24(%edi)
    movl 24(%ebp), %eax         # name ptr
    movl %eax, 28(%edi)
    movl $0xFFFFFFFF, 32(%edi)  # builtin = none
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lfn_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_builtin(id@8 u64) -> aval (callable builtin marker, T_FNON builtin=id)
    .globl a_builtin
a_builtin:
    pushl %ebp
    movl %esp, %ebp
    movl $48, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lbi2_none
    movl %eax, %edi
    movl $8, (%edi)
    movl $1, 4(%edi)
    movl $0, 8(%edi)
    movl $0, 12(%edi)
    movl $0, 16(%edi)           # entry = 0
    movl $2, 20(%edi)           # env = A_NONE
    movl $0xFFFF0000, 24(%edi)
    movl $0, 28(%edi)           # name = NULL
    movl 8(%ebp), %eax          # id lo32
    movl %eax, 32(%edi)
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lbi2_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_call(fn@8 aval, n@16 u64, argv@24 ptr4) -> aval
    .globl a_call
a_call:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lcl_none
    cmpl $8, (%esi)
    jne .Lcl_none
    cmpl $0xFFFFFFFF, 32(%esi)
    je .Lcl_fnpath
    # builtin: tail a_bi_call(id, n, argv)
    movl 32(%esi), %eax         # id
    pushl 24(%ebp)              # argv ptr
    pushl 20(%ebp)              # n hi
    pushl 16(%ebp)              # n lo
    pushl $0                    # id hi
    pushl %eax                  # id lo
    call a_bi_call
    addl $20, %esp
    popl %ebp
    ret
.Lcl_fnpath:
    pushl %esi                  # [ebp-4] fn obj
    movl 20(%ebp), %eax         # n hi must be 0
    testl %eax, %eax
    jnz .Lcl_none_pop
    movl 16(%ebp), %edx         # n
.Lcl_argloop:
    testl %edx, %edx
    jz .Lcl_argsdone
    decl %edx
    movl 24(%ebp), %eax         # argv base
    movl %edx, %esi             # idx (fn obj safe on stack)
    shll $3, %esi
    addl %esi, %eax             # &argv[i]
    pushl 4(%eax)               # hi
    pushl (%eax)                # lo
    jmp .Lcl_argloop
.Lcl_argsdone:
    movl -4(%ebp), %esi         # fn obj
    pushl 24(%esi)              # env hi
    pushl 20(%esi)              # env lo  -> av[0] = env (lowest addr)
    movl %esp, %eax
    addl $0x90000, %eax         # flat ptr to av[0]
    pushl %eax                  # arg0 = av ptr
    movl 16(%esi), %ecx         # entry (flat link VMA)
    # call *%ecx is a NEAR indirect call: the operand is an IP offset
    # within CS, not a linear address. CS base is 0x10000 (ljmp 0x1000:0
    # in _entry), so subtract selector<<4 to get the in-segment offset.
    movw %cs, %ax
    movzwl %ax, %eax
    shll $4, %eax
    subl %eax, %ecx
    call *%ecx
    addl $4, %esp               # drop av ptr
    # eax:edx hold the callee's aval return — compute the drop count in ecx
    # so the return value survives.
    movl 16(%ebp), %ecx
    incl %ecx
    shll $3, %ecx
    addl %ecx, %esp             # drop (n+1)*8 av bytes
    movl %ebp, %esp
    popl %ebp
    ret
.Lcl_none_pop:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    movl %ebp, %esp
    popl %ebp
    ret
.Lcl_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# ---- bitwise int ops (same skeleton as a_add_slow) ------------------------
    .globl a_band_slow
a_band_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lbw_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lbw_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx                  # a_hi
    popl %edx                  # a_lo
    andl %edx, %esi
    andl %ecx, %edi
    call .Lrebox
    popl %ebp
    ret
.Lbw_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_bor_slow
a_bor_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw2_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lbw2_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw2_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lbw2_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    orl %edx, %esi
    orl %ecx, %edi
    call .Lrebox
    popl %ebp
    ret
.Lbw2_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

    .globl a_bxor_slow
a_bxor_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw3_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lbw3_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lbw3_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lbw3_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    popl %ecx
    popl %edx
    xorl %edx, %esi
    xorl %ecx, %edi
    call .Lrebox
    popl %ebp
    ret
.Lbw3_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# ---- shifts (i64; count = r&63, or 64 if r doesn't fit u32) ----------------
# Stack layout inside: after unboxing l we push its i64 -> [esp]=l_lo,[esp+4]=l_hi
# a_shl_slow(l@8, r@16): l << s  (logical)
    .globl a_shl_slow
a_shl_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lshl_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lshl_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lshl_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lshl_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox               # l
    pushl %edi
    pushl %esi                 # [esp]=l_lo [esp+4]=l_hi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox               # r
    movl %esi, %eax
    andl $63, %eax
    testl %edi, %edi
    jz .Lshl_cnt
    movl $64, %eax
.Lshl_cnt:
    cmpl $32, %eax
    jb .Lshl_lt32
    je .Lshl_eq32
    cmpl $64, %eax
    jae .Lshl_zerores
    # 33..63: t = s-32 in 1..31; lo' = hi<<t ; hi' = hi>>(32-t) logical
    subl $32, %eax             # t
    movl %eax, %ecx
    movl 4(%esp), %esi         # l_hi
    shll %cl, %esi             # lo'
    movl $32, %ebx
    subl %eax, %ebx            # 32-t
    movl %ebx, %ecx
    movl 4(%esp), %edi         # l_hi
    shrl %cl, %edi             # hi'
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshl_eq32:
    movl 4(%esp), %esi         # lo' = hi
    xorl %edi, %edi            # hi' = 0
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshl_lt32:
    testl %eax, %eax
    jz .Lshl_id
    movl %eax, %ecx            # cl = s
    movl (%esp), %esi          # l_lo
    shll %cl, %esi
    movl $32, %ebx
    subl %eax, %ebx            # 32-s
    movl %ebx, %ecx
    movl 4(%esp), %edi         # l_hi
    shrl %cl, %edi             # hi >> (32-s)
    orl %edi, %esi             # lo'
    movl %eax, %ecx
    movl 4(%esp), %edi
    shll %cl, %edi             # hi' = hi<<s
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshl_id:
    movl (%esp), %esi
    movl 4(%esp), %edi
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshl_zerores:
    xorl %esi, %esi
    xorl %edi, %edi
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshl_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# a_shr_slow(l@8, r@16): l >> s  (arithmetic)
    .globl a_shr_slow
a_shr_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lshr_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lshr_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lshr_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lshr_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %edi
    pushl %esi                 # [esp]=l_lo [esp+4]=l_hi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    movl %esi, %eax
    andl $63, %eax
    testl %edi, %edi
    jz .Lshr_cnt
    movl $64, %eax
.Lshr_cnt:
    cmpl $32, %eax
    jb .Lshr_lt32
    je .Lshr_eq32
    cmpl $64, %eax
    jae .Lshr_sign
    # 33..63: t=s-32; lo' = hi sar t ; hi' = sign(hi)
    subl $32, %eax
    movl %eax, %ecx
    movl 4(%esp), %esi
    sarl %cl, %esi             # lo'
    movl 4(%esp), %edi
    sarl $31, %edi             # hi' = sign
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshr_eq32:
    movl 4(%esp), %esi         # lo' = hi
    movl 4(%esp), %edi
    sarl $31, %edi             # hi' = sign
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshr_lt32:
    testl %eax, %eax
    jz .Lshr_id
    movl %eax, %ecx            # cl = s
    movl (%esp), %esi
    movl %ecx, %ebx
    shrl %cl, %esi             # lo >>> s
    movl $32, %ecx
    subl %ebx, %ecx            # 32-s
    movl 4(%esp), %edi         # l_hi
    shll %cl, %edi             # hi << (32-s)
    orl %edi, %esi             # lo'
    movl %ebx, %ecx
    movl 4(%esp), %edi
    sarl %cl, %edi             # hi' = hi >> s (arith)
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshr_id:
    movl (%esp), %esi
    movl 4(%esp), %edi
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshr_sign:
    movl 4(%esp), %edi
    sarl $31, %edi             # hi' = sign ; lo' = 0
    xorl %esi, %esi
    addl $8, %esp
    call .Lrebox
    popl %ebp
    ret
.Lshr_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret


# a_pow(l@8, r@16): tagged ints -> l^r (r>=0, s32 wrap) or A_NONE
    .globl a_pow
a_pow:
    pushl %ebp
    movl %esp, %ebp
    movl 12(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lpw_none
    movl 8(%ebp), %eax
    testl $1, %eax
    jz .Lpw_none
    movl 20(%ebp), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lpw_none
    movl 16(%ebp), %eax
    testl $1, %eax
    jz .Lpw_none
    movl 8(%ebp), %esi
    movl 12(%ebp), %edi
    call .Lunbox
    pushl %esi
    pushl %edi
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    call .Lunbox
    # require both fit s32
    testl %edi, %edi
    jnz .Lpw_chkneg1
    cmpl $0x80000000, %esi
    jae .Lpw_none
    jmp .Lpw_baseok
.Lpw_chkneg1:
    cmpl $0xFFFFFFFF, %edi
    jne .Lpw_none
    cmpl $0x80000000, %esi
    jb .Lpw_none
.Lpw_baseok:                    # esi = base s32 (saved on stack? it's reg; push)
    pushl %esi                   # base
    # exp in esi:edi -> must be s32 nonneg
    testl %edi, %edi
    jnz .Lpw_expchk
    cmpl $0x80000000, %esi
    jae .Lpw_expnone
    testl %esi, %esi
    js .Lpw_expnone               # negative exp -> none
    jmp .Lpw_expok
.Lpw_expchk:
    cmpl $0xFFFFFFFF, %edi
    jne .Lpw_expnone
.Lpw_expnone:
    addl $4, %esp
    jmp .Lpw_none
.Lpw_expok:
    movl %esi, %ecx               # exp
    movl (%esp), %eax             # base
    movl $1, %edx                 # result
.Lpw_loop:
    testl %ecx, %ecx
    jz .Lpw_done
    imull %eax, %edx
    decl %ecx
    jmp .Lpw_loop
.Lpw_done:
    movl %edx, %esi
    xorl %edi, %edi
    call .Lrebox
    addl $4, %esp
    popl %ebp
    ret
.Lpw_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# ---- remaining runtime entry points ---------------------------------------

# .Lstreq: esi = name cstr, edi = literal cstr -> eax = 1/0. NUL-terminated
# compare; preserves esi,edi. clobbers eax,ecx? (uses al only + pushes)
.Lstreq:
    pushl %esi
    pushl %edi
.Lse_loop:
    movb (%esi), %al
    cmpb %al, (%edi)
    jne .Lse_no
    testb %al, %al
    jz .Lse_yes
    incl %esi
    incl %edi
    jmp .Lse_loop
.Lse_yes:
    movl $1, %eax
    jmp .Lse_out
.Lse_no:
    xorl %eax, %eax
.Lse_out:
    popl %edi
    popl %esi
    ret

# ---- a_getattr(obj@8 aval, name@16 cstr4) ---------------------------------
# Subset of the C runtime: T_DICT looks the name up as a str key; anything
# else yields A_NONE (C would panic; structs/modules are lowered elsewhere).
    .section .data
    .globl ga_key
ga_key: .long 0
    .long 0
    .section .text
    .globl a_getattr
a_getattr:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lga_none
    cmpl $4, (%esi)             # T_DICT only
    jne .Lga_none
    pushl %esi                  # [ebp-4] dict
    # strlen(name)
    movl 16(%ebp), %edi
.Lga_len:
    cmpb $0, (%edi)
    je .Lga_len_done
    incl %edi
    jmp .Lga_len
.Lga_len_done:
    movl 16(%ebp), %edx
    subl %edx, %edi             # edi = len
    # a_str_new(name, len): push len_hi, len_lo, ptr
    pushl $0
    pushl %edi
    movl 16(%ebp), %eax
    pushl %eax
    call a_str_new
    addl $12, %esp
    movl %eax, ga_key
    movl %edx, ga_key+4
    movl $ga_key, %edi
    movl -4(%ebp), %esi         # dict (.Ldict_find preserves esi/edi)
    call .Ldict_find
    testl %eax, %eax
    js .Lga_none2
    movl -4(%ebp), %ecx         # dict
    movl 16(%ecx), %edx         # pairs
    shll $4, %eax
    addl %eax, %edx
    movl 8(%edx), %eax          # v lo
    movl 12(%edx), %edx         # v hi
    jmp .Lga_out
.Lga_none2:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
.Lga_out:
    movl %ebp, %esp
    popl %ebp
    ret
.Lga_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# ---- a_slice(obj@8, lo@16, hi@24, step@32) -> aval --------------------------
# lo/hi/step are avals; A_NONE means "default". Bounds follow the C runtime:
# negative indices wrap by +n, results clamp to [lower,upper] where
# st>0 -> [0,n] dflts {0,n}, st<0 -> [-1,n-1] dflts {n-1,-1}.
# Strings are sliced BY BYTE (the i8086 str subset is byte-oriented, same as
# a_getitem); lists/tuples produce a fresh same-type AArr.
# Helpers:
#   .Lraw_s32: edi=flat aval ptr -> eax=s32, edx=1 ok / 0 bad (no none check)
#   .Loptint:  edi=flat aval ptr, esi=default -> eax, edx ok (none->default)
#   .Lbound:   edi=flat aval ptr, esi=n ; stack args dflt@8 lower@12
#              upper@16 n@20 -> eax=index edx=1 ok / 0 bad
.Lraw_s32:
    pushl %edi
    movl 4(%edi), %eax
    andl $0xFFFF0000, %eax
    cmpl $0xFFFF0000, %eax
    jne .Lrs_bad
    movl (%edi), %ecx
    testl $1, %ecx
    jz .Lrs_bad
    movl (%edi), %esi
    movl 4(%edi), %edi
    call .Lunbox               # esi=i_lo edi=i_hi ; clobbers eax,ecx,edx
    testl %edi, %edi
    jnz .Lrs_neg
    cmpl $0x80000000, %esi
    jae .Lrs_bad               # >= 2^31 : outside s32
    movl %esi, %eax
    movl $1, %edx
    popl %edi
    ret
.Lrs_neg:
    cmpl $0xFFFFFFFF, %edi
    jne .Lrs_bad
    movl %esi, %eax
    movl $1, %edx
    popl %edi
    ret
.Lrs_bad:
    xorl %edx, %edx
    xorl %eax, %eax
    popl %edi
    ret

.Loptint:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] default
    movl (%edi), %eax
    cmpl $2, %eax
    jne .Loi_go
    movl 4(%edi), %ecx
    cmpl $0xFFFF0000, %ecx
    je .Loi_dflt
.Loi_go:
    call .Lraw_s32
    jmp .Loi_out
.Loi_dflt:
    movl -4(%ebp), %eax
    movl $1, %edx
.Loi_out:
    movl %ebp, %esp
    popl %ebp
    ret

.Lbound:
    pushl %ebp
    movl %esp, %ebp
    movl (%edi), %eax
    cmpl $2, %eax
    jne .Lb_chk
    movl 4(%edi), %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lb_chk
    movl 8(%ebp), %eax          # default: already final, no adjust
    movl $1, %edx
    jmp .Lb_out
.Lb_chk:
    call .Lraw_s32
    testl %edx, %edx
    jz .Lb_bad
    testl %eax, %eax
    jns .Lb_cl
    addl 20(%ebp), %eax         # += n
.Lb_cl:
    movl 12(%ebp), %ecx         # lower
    cmpl %ecx, %eax
    jge 1f
    movl %ecx, %eax
1:  movl 16(%ebp), %ecx         # upper
    cmpl %ecx, %eax
    jle 2f
    movl %ecx, %eax
2:  movl $1, %edx
    jmp .Lb_out
.Lb_bad:
    xorl %edx, %edx
    xorl %eax, %eax
.Lb_out:
    movl %ebp, %esp
    popl %ebp
    ret

    .globl a_slice
a_slice:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lsl_fail
    movl %esi, %ebx             # obj
    movl (%ebx), %ecx
    cmpl $1, %ecx
    je .Lsl_go
    cmpl $2, %ecx
    je .Lsl_go
    cmpl $3, %ecx
    je .Lsl_go
    jmp .Lsl_fail
.Lsl_go:
    movl 4(%ebx), %eax          # len (bytes for str, elems for arr)
    pushl %ebx                  # [esp+8] obj
    pushl %eax                  # [esp+4] n
    # step: aval @32 -> flat ebp+0x90020 ; default 1
    movl %ebp, %edi
    addl $0x90020, %edi
    movl $1, %esi
    call .Loptint
    testl %edx, %edx
    jz .Lsl_fail
    testl %eax, %eax
    jz .Lsl_fail                # step == 0
    pushl %eax                  # [esp] step, [esp+4] n, [esp+8] obj
    movl (%esp), %ecx           # step
    jns .Lsl_pos
    # ---- negative step bounds ----
    # lo: dflt=n-1 lower=-1 upper=n-1
    movl 4(%esp), %eax
    movl %eax, %ecx
    decl %ecx                   # n-1
    pushl %eax                  # arg4 n
    pushl %ecx                  # upper n-1
    pushl $0xFFFFFFFF           # lower -1
    pushl %ecx                  # dflt n-1
    movl %ebp, %edi
    addl $0x90010, %edi         # &lo
    call .Lbound
    addl $16, %esp
    testl %edx, %edx
    jz .Lsl_fail
    movl %eax, %ebx             # start
    # hi: dflt=-1 lower=-1 upper=n-1
    movl 4(%esp), %eax          # n
    movl %eax, %ecx
    decl %ecx                   # n-1
    pushl %eax                  # arg4 n
    pushl %ecx                  # upper n-1
    pushl $0xFFFFFFFF           # lower -1
    pushl $0xFFFFFFFF           # dflt -1
    movl %ebp, %edi
    addl $0x90018, %edi         # &hi
    call .Lbound
    addl $16, %esp
    testl %edx, %edx
    jz .Lsl_fail
    # esi=start eax=stop step=(%esp) n=4(%esp)
    movl %ebx, %edx
    subl %eax, %edx             # d = start - stop
    cmpl %eax, %ebx
    jle .Lsl_cnt0               # start <= stop -> empty
    movl (%esp), %ecx
    negl %ecx                   # a = -step (>0)
    decl %edx                   # d-1
    movl %edx, %eax             # dividend lo = d-1
    xorl %edx, %edx             # dividend hi = 0
    divl %ecx                   # (d-1)/a
    incl %eax
    jmp .Lsl_havecnt
.Lsl_pos:
    # lo: dflt=0 lower=0 upper=n
    movl 4(%esp), %eax
    pushl %eax                  # n
    pushl %eax                  # upper n
    pushl $0                    # lower 0
    pushl $0                    # dflt 0
    movl %ebp, %edi
    addl $0x90010, %edi
    call .Lbound
    addl $16, %esp
    testl %edx, %edx
    jz .Lsl_fail
    movl %eax, %ebx             # start
    # hi: dflt=n lower=0 upper=n
    movl 4(%esp), %eax
    pushl %eax
    pushl %eax
    pushl $0
    pushl %eax
    movl %ebp, %edi
    addl $0x90018, %edi
    call .Lbound
    addl $16, %esp
    testl %edx, %edx
    jz .Lsl_fail
    # stop <= start -> empty ; else cnt=(stop-start-1)/step+1
    cmpl %ebx, %eax
    jle .Lsl_cnt0
    subl %ebx, %eax
    decl %eax
    movl (%esp), %ecx           # step > 0
    xorl %edx, %edx
    divl %ecx
    incl %eax
    jmp .Lsl_havecnt
.Lsl_cnt0:
    xorl %eax, %eax
.Lsl_havecnt:
    pushl %eax                  # [esp] cnt
    pushl %ebx                  # [esp] start, +4 cnt, +8 step, +12 n, +16 obj
    movl 16(%esp), %eax         # obj
    movl (%eax), %ecx
    cmpl $1, %ecx
    je .Lsl_strb
    jmp .Lsl_arrb
# ---- string slice: byte range -> new T_STR ----
.Lsl_strb:
    movl 4(%esp), %ebx          # cnt
    movl %ebx, %eax
    addl $17, %eax              # 16 hdr + cnt + NUL (+ .Lalloc pad)
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lsl_fail
    movl %eax, %esi             # new str obj
    movl $1, (%esi)
    movl 4(%esp), %eax          # cnt
    movl %eax, 4(%esi)
    incl %eax
    movl %eax, 8(%esi)
    movl $0, 12(%esi)
    movl %esi, %edi
    addl $16, %edi              # dst cursor
    movl 16(%esp), %ecx         # old obj
    addl $16, %ecx              # data base
    movl (%esp), %eax           # start
    addl %eax, %ecx             # src cursor
    movl 4(%esp), %ebx          # remaining = cnt
.Lsl_sb_loop:
    testl %ebx, %ebx
    jz .Lsl_sb_done
    movb (%ecx), %al
    movb %al, (%edi)
    incl %edi
    movl 8(%esp), %eax          # step
    addl %eax, %ecx
    decl %ebx
    jmp .Lsl_sb_loop
.Lsl_sb_done:
    movb $0, (%edi)
    movl %esi, %eax
    movl $0xFFFF0000, %edx
    jmp .Lsl_out
# ---- list/tuple slice: fresh AArr, same type, inline items@40 ----
.Lsl_arrb:
    movl 4(%esp), %eax          # cnt
    shll $3, %eax
    addl $40, %eax
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lsl_fail
    movl %eax, %esi             # new arr obj
    movl 16(%esp), %eax         # old obj
    movl (%eax), %ecx           # type 2/3
    movl %ecx, (%esi)
    movl 4(%esp), %eax          # cnt
    movl %eax, 4(%esi)
    movl %eax, 8(%esi)          # cap = cnt
    movl $0, 12(%esi)
    movl %esi, %ecx
    addl $40, %ecx
    movl %ecx, 16(%esi)         # items = obj+40
    movl $0, 24(%esi)           # iitems
    movl $0, 32(%esi)           # flags
    movl %esi, %edi
    addl $40, %edi              # dst cursor
    movl 16(%esp), %eax         # old obj
    movl 16(%eax), %ecx         # items ptr
    movl (%esp), %eax           # start
    shll $3, %eax
    addl %eax, %ecx             # src cursor (byte addr)
    movl 4(%esp), %ebx          # remaining
.Lsl_ab_loop:
    testl %ebx, %ebx
    jz .Lsl_ab_done
    movl (%ecx), %eax
    movl %eax, (%edi)
    movl 4(%ecx), %eax
    movl %eax, 4(%edi)
    addl $8, %edi
    movl 8(%esp), %eax          # step
    shll $3, %eax
    addl %eax, %ecx
    decl %ebx
    jmp .Lsl_ab_loop
.Lsl_ab_done:
    movl %esi, %eax
    movl $0xFFFF0000, %edx
.Lsl_out:
    movl %ebp, %esp
    popl %ebp
    ret
.Lsl_fail:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    movl %ebp, %esp
    popl %ebp
    ret

# ---- a_str_joinsep(sep@8 aval, n@16 u64, items@24 ptr4) -> aval ------------
# String interpolation lowering: parts arrive as avals (codegen already ran
# a_str on expr parts, so items are T_STR/T_CHAR; non-str leftovers are
# converted via a_str here for robustness). sep may be any str-ish aval.
    .globl a_str_joinsep
a_str_joinsep:
    pushl %ebp
    movl %esp, %ebp
    movl 20(%ebp), %eax         # n hi32
    testl %eax, %eax
    jnz .Ljs_none
    movl 16(%ebp), %eax         # n lo
    cmpl $65536, %eax           # sanity cap
    ja .Ljs_none
    pushl %eax                  # [ebp-4] n
    movl 24(%ebp), %eax         # items ptr
    pushl %eax                  # [ebp-8] base
    pushl $0                    # [ebp-12] i
    # copy items into a scratch aval array so normalization never mutates
    # the caller's collection; -8(%ebp) is then repointed at the scratch.
    movl -4(%ebp), %ebx
    shll $3, %ebx               # n*8 bytes
    call .Lalloc
    testl %eax, %eax
    jz .Ljs_none2
    movl %eax, %esi             # scratch
    movl %esi, -8(%ebp)         # base = scratch
    # copy n avals: src = arg items ptr (24(%ebp)), dst = scratch
    movl 24(%ebp), %ecx         # src cursor
    movl -4(%ebp), %edx
    shll $3, %edx               # nbytes
.Ljs_cp:
    testl %edx, %edx
    jz .Ljs_cp_done
    movl (%ecx), %eax
    movl %eax, (%esi)
    movl 4(%ecx), %eax
    movl %eax, 4(%esi)
    addl $8, %ecx
    addl $8, %esi
    subl $8, %edx
    jmp .Ljs_cp
.Ljs_cp_done:
    xorl %esi, %esi
    # ---- pass 1: normalize non-str items via a_str ----
.Ljs_norm:
    movl -12(%ebp), %eax
    cmpl -4(%ebp), %eax
    jae .Ljs_normdone
    movl -8(%ebp), %edx
    movl %eax, %ecx
    shll $3, %ecx
    addl %ecx, %edx             # &items[i]
    pushl 4(%edx)
    pushl (%edx)
    call .Lobjptr
    addl $8, %esp
    testl %esi, %esi
    jz .Ljs_conv
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Ljs_next
    cmpl $12, %ecx
    je .Ljs_next
.Ljs_conv:
    movl -8(%ebp), %eax
    movl -12(%ebp), %ecx
    shll $3, %ecx
    addl %ecx, %eax             # &items[i]
    pushl 4(%eax)
    pushl (%eax)
    call a_str
    addl $8, %esp
    pushl %edx                  # aval hi
    pushl %eax                  # aval lo
    movl -8(%ebp), %ecx
    movl -12(%ebp), %edx
    shll $3, %edx
    addl %edx, %ecx             # &items[i]
    popl %edx                   # lo
    movl %edx, (%ecx)
    popl %edx                   # hi
    movl %edx, 4(%ecx)
.Ljs_next:
    incl -12(%ebp)
    jmp .Ljs_norm
.Ljs_normdone:
    # ---- sep -> sp/sn ----
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Ljs_nosep
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Ljs_sepok
    cmpl $12, %ecx
    je .Ljs_sepok
    jmp .Ljs_nosep
.Ljs_sepok:
    movl 4(%esi), %eax          # sn
    movl %esi, %edx
    addl $16, %edx              # sp
    jmp .Ljs_savesep
.Ljs_nosep:
    xorl %eax, %eax
    xorl %edx, %edx
.Ljs_savesep:
    pushl %edx                  # [ebp-16] sp
    pushl %eax                  # [ebp-20] sn
    pushl $0                    # [ebp-24] total
    # ---- pass 2: total bytes ----
    movl $0, -12(%ebp)          # i = 0
.Ljs_szloop:
    movl -12(%ebp), %eax
    cmpl -4(%ebp), %eax
    jae .Ljs_szdone
    movl -8(%ebp), %edx
    movl %eax, %ecx
    shll $3, %ecx
    addl %ecx, %edx             # &items[i]
    movl (%edx), %ecx           # lo
    movl 4(%edx), %edx          # hi
    movl %edx, %esi
    andl $0xFFFF0000, %esi
    cmpl $0xFFFF0000, %esi
    jne .Ljs_sznext
    testl $1, %ecx
    jnz .Ljs_sznext
    cmpl $2, %ecx
    je .Ljs_sznext
    cmpl $4, %ecx
    je .Ljs_sznext
    cmpl $6, %ecx
    je .Ljs_sznext
    testl $0xF, %ecx
    jnz .Ljs_sznext
    testl %ecx, %ecx
    jz .Ljs_sznext
    movl (%ecx), %esi
    cmpl $1, %esi
    je .Ljs_szadd
    cmpl $12, %esi
    je .Ljs_szadd
    jmp .Ljs_sznext
.Ljs_szadd:
    movl 4(%ecx), %edx
    addl %edx, -24(%ebp)
.Ljs_sznext:
    incl -12(%ebp)
    jmp .Ljs_szloop
.Ljs_szdone:
    # total += sn * (n-1) when n > 0
    movl -4(%ebp), %eax
    testl %eax, %eax
    jz .Ljs_sepcnt_done
    decl %eax
    imull -20(%ebp), %eax       # sn * (n-1)
    addl %eax, -24(%ebp)
.Ljs_sepcnt_done:
    # ---- alloc + fill ----
    movl -24(%ebp), %ebx        # total
    movl %ebx, %eax
    addl $17, %eax
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Ljs_none2
    movl %eax, %ebx             # new str obj (ebx free: n on stack)
    movl $1, (%ebx)
    movl -24(%ebp), %eax
    movl %eax, 4(%ebx)
    incl %eax
    movl %eax, 8(%ebx)
    movl $0, 12(%ebx)
    movl %ebx, %edi
    addl $16, %edi              # dst cursor
    movl $0, -12(%ebp)          # i = 0
.Ljs_fill:
    movl -12(%ebp), %eax
    cmpl -4(%ebp), %eax
    jae .Ljs_filldone
    testl %eax, %eax
    jz .Ljs_fitem
    movl -20(%ebp), %ecx        # sn
    testl %ecx, %ecx
    jz .Ljs_fitem
    movl -16(%ebp), %eax        # sp
    # copy sn bytes
.Ljs_sepcp:
    testl %ecx, %ecx
    jz .Ljs_fitem
    movb (%eax), %dl
    movb %dl, (%edi)
    incl %eax
    incl %edi
    decl %ecx
    jmp .Ljs_sepcp
.Ljs_fitem:
    movl -8(%ebp), %eax
    movl -12(%ebp), %ecx
    shll $3, %ecx
    addl %ecx, %eax             # &items[i]
    movl (%eax), %ecx           # lo
    movl 4(%eax), %edx          # hi
    movl %edx, %esi
    andl $0xFFFF0000, %esi
    cmpl $0xFFFF0000, %esi
    jne .Ljs_fnext
    testl $1, %ecx
    jnz .Ljs_fnext
    cmpl $2, %ecx
    je .Ljs_fnext
    cmpl $4, %ecx
    je .Ljs_fnext
    cmpl $6, %ecx
    je .Ljs_fnext
    testl $0xF, %ecx
    jnz .Ljs_fnext
    testl %ecx, %ecx
    jz .Ljs_fnext
    movl (%ecx), %esi
    cmpl $1, %esi
    je .Ljs_fstr
    cmpl $12, %esi
    je .Ljs_fstr
    jmp .Ljs_fnext
.Ljs_fstr:
    movl 4(%ecx), %esi          # len
    movl %ecx, %eax
    addl $16, %eax              # src cursor
.Ljs_fcp:
    testl %esi, %esi
    jz .Ljs_fnext
    movb (%eax), %dl
    movb %dl, (%edi)
    incl %eax
    incl %edi
    decl %esi
    jmp .Ljs_fcp
.Ljs_fnext:
    incl -12(%ebp)
    jmp .Ljs_fill
.Ljs_filldone:
    movb $0, (%edi)
    movl %ebx, %eax
    movl $0xFFFF0000, %edx
    jmp .Ljs_out
.Ljs_none2:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
.Ljs_out:
    movl %ebp, %esp
    popl %ebp
    ret
.Ljs_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# ---- a_repr(v@8) -> aval : repr via 16KiB scratch buffer -------------------
# .Lrepr_val: esi -> aval ptr; appends repr text to [repr_pos,repr_lim).
# Mirrors .Lb_paval's subset (int/none/bool/str-quoted/list/tuple/set/dict).
    .section .data
    .globl repr_buf
    .globl repr_pos
    .globl repr_lim
    .globl repr_arg
repr_buf: .long 0
repr_pos: .long 0
repr_lim: .long 0
repr_arg: .long 0
    .long 0
    .section .text
    .globl a_repr
a_repr:
    pushl %ebp
    movl %esp, %ebp
    movl repr_buf, %eax
    testl %eax, %eax
    jnz .Lrp_have
    movl $16384, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lrp_none
    movl %eax, repr_buf
    movl %eax, repr_pos
    movl %eax, %ecx
    addl $16384, %ecx
    movl %ecx, repr_lim
.Lrp_have:
    movl repr_buf, %eax
    movl %eax, repr_pos          # reset cursor
    movl 8(%ebp), %eax           # copy arg aval -> repr_arg (flat slot)
    movl %eax, repr_arg
    movl 12(%ebp), %eax
    movl %eax, repr_arg+4
    movl $repr_arg, %esi
    call .Lrepr_val
    # a_str_new(repr_buf, len = pos - buf)
    movl repr_pos, %eax
    subl repr_buf, %eax
    pushl $0                     # len hi
    pushl %eax                   # len lo
    movl repr_buf, %eax
    pushl %eax                   # ptr
    call a_str_new
    addl $12, %esp
    popl %ebp
    ret
.Lrp_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret

# .Lrepr_emit: al -> buffer (drop on overflow). clobbers eax,ecx,edx.
.Lrepr_emit:
    movl repr_pos, %ecx
    movl repr_lim, %edx
    cmpl %edx, %ecx
    jae .Lre_done
    movb %al, (%ecx)
    incl repr_pos
.Lre_done:
    ret

# .Lrepr_n: esi=ptr ebx=len -> append bytes. clobbers eax,ecx,edx,esi,ebx.
.Lrepr_n:
.Lrn_loop:
    testl %ebx, %ebx
    jz .Lrn_done
    movb (%esi), %al
    call .Lrepr_emit
    incl %esi
    decl %ebx
    jmp .Lrn_loop
.Lrn_done:
    ret

# .Lrepr_val: esi -> aval (8B) ; appends repr. Recursive.
.Lrepr_val:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] aval ptr
    movl (%esi), %eax
    movl 4(%esi), %edx
    movl %edx, %ecx
    andl $0xFFFF0000, %ecx
    cmpl $0xFFFF0000, %ecx
    jne .Lrv_q
    testl $1, %eax
    jnz .Lrv_int
    cmpl $2, %eax
    je .Lrv_none
    cmpl $4, %eax
    je .Lrv_false
    cmpl $6, %eax
    je .Lrv_true
    testl $0xF, %eax
    jnz .Lrv_q
    testl %eax, %eax
    jz .Lrv_q
    movl %eax, %ebx             # obj
    movl (%ebx), %ecx
    cmpl $1, %ecx
    je .Lrv_str
    cmpl $12, %ecx
    je .Lrv_str
    cmpl $2, %ecx
    je .Lrv_arr
    cmpl $3, %ecx
    je .Lrv_arr
    cmpl $5, %ecx
    je .Lrv_set
    cmpl $4, %ecx
    je .Lrv_dict
    jmp .Lrv_q
.Lrv_int:
    movl -4(%ebp), %ecx
    movl (%ecx), %esi
    movl 4(%ecx), %edi
    call .Lunbox
    call .Lint_fmt             # eax=ptr or 0, ebx=len
    testl %eax, %eax
    jz .Lrv_q
    movl %eax, %esi
    call .Lrepr_n
    jmp .Lrv_out
.Lrv_none:
    movl $str_none, %esi
    movl $4, %ebx
    call .Lrepr_n
    jmp .Lrv_out
.Lrv_false:
    movl $str_false, %esi
    movl $5, %ebx
    call .Lrepr_n
    jmp .Lrv_out
.Lrv_true:
    movl $str_true, %esi
    movl $4, %ebx
    call .Lrepr_n
    jmp .Lrv_out
.Lrv_str:                      # ebx = obj ; "..." quoted
    movb $'"', %al
    call .Lrepr_emit
    movl %ebx, %ecx
    addl $16, %ecx
    movl %ecx, %esi             # data ptr
    movl 4(%ebx), %ebx          # len
    call .Lrepr_n
    movb $'"', %al
    call .Lrepr_emit
    jmp .Lrv_out
.Lrv_arr:                      # ebx = T_LIST/T_TUPLE ; "[a, b]"
    pushl %ebx                  # [ebp-8] obj
    pushl $0                    # [ebp-12] i
    movb $'[', %al
    call .Lrepr_emit
.Lrva_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lrva_close
    testl %eax, %eax
    jz .Lrva_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lrepr_n
.Lrva_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx         # items base
    movl -12(%ebp), %eax
    shll $3, %eax
    addl %eax, %ecx             # &items[i]
    movl %ecx, %esi
    call .Lrepr_val
    incl -12(%ebp)
    jmp .Lrva_loop
.Lrva_close:
    movb $']', %al
    call .Lrepr_emit
    jmp .Lrv_out
.Lrv_set:                      # ebx = T_SET ; "{a, b}"
    pushl %ebx
    pushl $0
    movb $'{', %al
    call .Lrepr_emit
.Lrvs_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lrvs_close
    testl %eax, %eax
    jz .Lrvs_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lrepr_n
.Lrvs_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx
    movl -12(%ebp), %eax
    shll $3, %eax
    addl %eax, %ecx
    movl %ecx, %esi
    call .Lrepr_val
    incl -12(%ebp)
    jmp .Lrvs_loop
.Lrvs_close:
    movb $'}', %al
    call .Lrepr_emit
    jmp .Lrv_out
.Lrv_dict:                     # ebx = T_DICT ; "{k: v}"
    pushl %ebx
    pushl $0
    movb $'{', %al
    call .Lrepr_emit
.Lrvd_loop:
    movl -12(%ebp), %eax
    movl -8(%ebp), %edx
    cmpl 4(%edx), %eax
    jae .Lrvd_close
    testl %eax, %eax
    jz .Lrvd_nosep
    movl $str_comma, %esi
    movl $2, %ebx
    call .Lrepr_n
.Lrvd_nosep:
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx         # pairs base
    movl -12(%ebp), %eax
    shll $4, %eax
    addl %eax, %ecx             # &pair[i].k
    movl %ecx, %esi
    call .Lrepr_val
    movl $str_colon, %esi
    movl $2, %ebx
    call .Lrepr_n
    movl -8(%ebp), %edx
    movl 16(%edx), %ecx
    movl -12(%ebp), %eax
    shll $4, %eax
    addl $8, %eax
    addl %eax, %ecx             # &pair[i].v
    movl %ecx, %esi
    call .Lrepr_val
    incl -12(%ebp)
    jmp .Lrvd_loop
.Lrvd_close:
    movb $'}', %al
    call .Lrepr_emit
    jmp .Lrv_out
.Lrv_q:
    movb $'?', %al
    call .Lrepr_emit
.Lrv_out:
    movl %ebp, %esp
    popl %ebp
    ret

# ---- a_method(obj@8 aval, name@16 cstr4, n@20 u64, nkw@28 u64, argv@36 ptr4)
# Subset of the C method dispatch. seq (T_LIST=2/T_TUPLE=3): contains, count,
# first, last, join, copy + list-only append, pop, clear, reverse.
# str (T_STR=1): upper, lower, startswith, endswith, find, count, join.
# dict (T_DICT=4): get, contains, clear. set (T_SET=5): contains, add.
# Anything else -> A_NONE. Frame locals: [ebp-4]=obj ptr, then per-group
# pushes for len/items/i/cnt as noted.
    .section .rodata
mn_append:   .asciz "append"
mn_pop:      .asciz "pop"
mn_clear:    .asciz "clear"
mn_contains: .asciz "contains"
mn_count:    .asciz "count"
mn_first:    .asciz "first"
mn_last:     .asciz "last"
mn_join:     .asciz "join"
mn_copy:     .asciz "copy"
mn_reverse:  .asciz "reverse"
mn_upper:    .asciz "upper"
mn_lower:    .asciz "lower"
mn_startswith: .asciz "startswith"
mn_endswith:   .asciz "endswith"
mn_find:     .asciz "find"
mn_get:      .asciz "get"
mn_add:      .asciz "add"
    .section .text
    .globl a_method
a_method:
    pushl %ebp
    movl %esp, %ebp
    movl 28(%ebp), %eax         # nkw lo
    orl 32(%ebp), %eax          # nkw hi
    jnz .Lmt_none
    movl 8(%ebp), %eax
    movl 12(%ebp), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lmt_none
    pushl %esi                  # [ebp-4] obj
    movl (%esi), %ecx
    cmpl $2, %ecx
    je .Lmt_seq
    cmpl $3, %ecx
    je .Lmt_seq
    cmpl $1, %ecx
    je .Lmt_str
    cmpl $4, %ecx
    je .Lmt_dict
    cmpl $5, %ecx
    je .Lmt_set
    jmp .Lmt_none_pop

# ---------------- seq (list/tuple) ----------------------------------------
# locals: -4 obj, -8 len, -12 items
.Lmt_seq:
    movl -4(%ebp), %eax
    movl 4(%eax), %ecx
    pushl %ecx                  # [ebp-8] len
    movl 16(%eax), %ecx
    pushl %ecx                  # [ebp-12] items
    movl 16(%ebp), %esi
    movl $mn_contains, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_contains
    movl 16(%ebp), %esi
    movl $mn_count, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_count
    movl 16(%ebp), %esi
    movl $mn_first, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_first
    movl 16(%ebp), %esi
    movl $mn_last, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_last
    movl 16(%ebp), %esi
    movl $mn_join, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_sjoin
    movl 16(%ebp), %esi
    movl $mn_copy, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_scopy
    movl -4(%ebp), %eax
    cmpl $2, (%eax)             # mutators: T_LIST only
    jne .Lmt_none_pop
    movl 16(%ebp), %esi
    movl $mn_append, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_append
    movl 16(%ebp), %esi
    movl $mn_pop, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_pop
    movl 16(%ebp), %esi
    movl $mn_clear, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_clear
    movl 16(%ebp), %esi
    movl $mn_reverse, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_reverse
    jmp .Lmt_none_pop

# contains(v): n==1 -> scan items with .Leqkey -> A_TRUE/A_FALSE
.Lmt_contains:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    pushl $0                    # [ebp-16] i
.Lmc_loop:
    movl -16(%ebp), %eax
    cmpl -8(%ebp), %eax
    jae .Lmc_false
    movl -12(%ebp), %ecx        # items
    movl %eax, %edx
    shll $3, %edx
    addl %ecx, %edx             # &items[i]
    movl %edx, %esi             # A
    movl 36(%ebp), %edi         # B = &argv[0]
    call .Leqkey
    testl %eax, %eax
    jnz .Lmc_true
    incl -16(%ebp)
    jmp .Lmc_loop
.Lmc_true:
    movl $6, %eax
    movl $0xFFFF0000, %edx
    jmp .Lmt_out
.Lmc_false:
    movl $4, %eax
    movl $0xFFFF0000, %edx
    jmp .Lmt_out

# count(v): n==1 -> int
.Lmt_count:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    pushl $0                    # [ebp-16] i
    pushl $0                    # [ebp-20] cnt
.Lmct_loop:
    movl -16(%ebp), %eax
    cmpl -8(%ebp), %eax
    jae .Lmt_cntdone
    movl -12(%ebp), %ecx
    movl %eax, %edx
    shll $3, %edx
    addl %ecx, %edx
    movl %edx, %esi
    movl 36(%ebp), %edi
    call .Leqkey
    testl %eax, %eax
    jz .Lmct_next
    incl -20(%ebp)
.Lmct_next:
    incl -16(%ebp)
    jmp .Lmct_loop
.Lmt_cntdone:
    movl -20(%ebp), %eax
    call .Lmksmall
    jmp .Lmt_out

# first()/last(): n==0 -> a_some_new(elem) or A_NONE when empty
.Lmt_first:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    cmpl $0, -8(%ebp)
    je .Lmt_none_pop
    xorl %esi, %esi             # idx = 0
    jmp .Lmt_fl_go
.Lmt_last:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    cmpl $0, -8(%ebp)
    je .Lmt_none_pop
    movl -8(%ebp), %esi
    decl %esi                   # idx = len-1
.Lmt_fl_go:
    movl -12(%ebp), %edx        # items
    movl %esi, %ecx
    shll $3, %ecx
    addl %ecx, %edx             # &elem
    movl 4(%edx), %eax          # hi
    pushl %eax
    movl (%edx), %eax           # lo
    pushl %eax
    call a_some_new
    addl $8, %esp
    jmp .Lmt_out

# join(sep): n==1 -> a_str_joinsep(argv[0], len, items)
.Lmt_sjoin:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl -12(%ebp), %ecx        # items
    movl -8(%ebp), %edx         # len
    pushl %ecx                  # items (arg2)
    pushl $0                    # n hi
    pushl %edx                  # n lo
    movl 36(%ebp), %eax         # argv base
    pushl 4(%eax)               # sep hi
    pushl (%eax)                # sep lo
    call a_str_joinsep
    addl $20, %esp
    jmp .Lmt_out

# copy(): n==0 -> fresh AArr same type, items copied
.Lmt_scopy:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    movl -8(%ebp), %eax
    shll $3, %eax
    addl $40, %eax
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lmt_none_pop
    movl %eax, %esi             # new obj
    movl -4(%ebp), %eax
    movl (%eax), %ecx           # type
    movl %ecx, (%esi)
    movl -8(%ebp), %ecx
    movl %ecx, 4(%esi)
    movl %ecx, 8(%esi)
    movl $0, 12(%esi)
    movl %esi, %ecx
    addl $40, %ecx
    movl %ecx, 16(%esi)
    movl $0, 24(%esi)
    movl $0, 32(%esi)
    movl %esi, %edi
    addl $40, %edi              # dst
    movl -12(%ebp), %ecx        # src items
    movl -8(%ebp), %edx
    shll $3, %edx               # nbytes
.Lmcp_loop:
    testl %edx, %edx
    jz .Lmcp_done
    movl (%ecx), %eax
    movl %eax, (%edi)
    movl 4(%ecx), %eax
    movl %eax, 4(%edi)
    addl $8, %ecx
    addl $8, %edi
    subl $8, %edx
    jmp .Lmcp_loop
.Lmcp_done:
    movl %esi, %eax
    movl $0xFFFF0000, %edx
    jmp .Lmt_out

# append(v): T_LIST, n==1 -> ensure + store, returns A_NONE
.Lmt_append:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %esi
    call .Larr_ensure
    testl %eax, %eax
    jz .Lmt_none_pop
    movl -4(%ebp), %esi
    movl 4(%esi), %ecx
    movl 16(%esi), %edx
    shll $3, %ecx
    addl %ecx, %edx
    movl 36(%ebp), %eax         # argv base
    movl (%eax), %ecx
    movl %ecx, (%edx)
    movl 4(%eax), %ecx
    movl %ecx, 4(%edx)
    incl 4(%esi)
    jmp .Lmt_none_pop

# pop([i]): T_LIST, n==0 or 1 -> removed elem or A_NONE
.Lmt_pop:
    movl 20(%ebp), %eax         # n lo
    cmpl $2, %eax
    ja .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    cmpl $0, -8(%ebp)
    je .Lmt_none_pop
    movl 20(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_pop_idx
    movl -8(%ebp), %eax
    decl %eax                   # idx = len-1
    jmp .Lmt_pop_have
.Lmt_pop_idx:
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 36(%ebp), %edi         # &argv[0]
    movl (%edi), %esi
    movl 4(%edi), %edi
    call .Lunbox
    testl %edi, %edi
    jnz .Lmt_pop_neg
    cmpl $0x80000000, %esi
    jae .Lmt_badargs
    movl %esi, %eax
    jmp .Lmt_pop_chk
.Lmt_pop_neg:
    cmpl $0xFFFFFFFF, %edi
    jne .Lmt_badargs
    movl %esi, %eax
.Lmt_pop_chk:
    testl %eax, %eax
    jns .Lmt_pop_v
    addl -8(%ebp), %eax         # negative: += len
.Lmt_pop_v:
    cmpl $0, %eax
    jl .Lmt_badargs
    cmpl -8(%ebp), %eax
    jge .Lmt_badargs
.Lmt_pop_have:
    pushl %eax                  # [ebp-16] idx
    # elem aval -> push hi,lo (top=lo)
    movl -12(%ebp), %ecx
    movl -16(%ebp), %edx
    shll $3, %edx
    addl %edx, %ecx             # &items[idx]
    movl 4(%ecx), %eax
    pushl %eax                  # hi
    movl (%ecx), %eax
    pushl %eax                  # lo
    # memmove items[idx+1..len) down one slot
    movl -12(%ebp), %edi
    movl -16(%ebp), %eax
    incl %eax
    shll $3, %eax
    addl %eax, %edi             # src = items+(idx+1)*8
    movl -12(%ebp), %edx
    movl -16(%ebp), %eax
    shll $3, %eax
    addl %eax, %edx             # dst = items+idx*8
    movl -8(%ebp), %eax         # len
    subl -16(%ebp), %eax
    decl %eax                   # count = len-idx-1
    shll $3, %eax               # bytes
.Lmtp_loop:
    testl %eax, %eax
    jz .Lmtp_done
    movl (%edi), %ecx
    movl %ecx, (%edx)
    movl 4(%edi), %ecx
    movl %ecx, 4(%edx)
    addl $8, %edi
    addl $8, %edx
    subl $8, %eax
    jmp .Lmtp_loop
.Lmtp_done:
    movl -4(%ebp), %ecx
    decl 4(%ecx)                # obj->len--
    popl %eax                   # lo
    popl %edx                   # hi
    jmp .Lmt_out

# clear(): T_LIST, n==0
.Lmt_clear:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %eax
    movl $0, 4(%eax)
    jmp .Lmt_none_pop

# reverse(): T_LIST, n==0 ; swap items[i] <-> items[len-1-i]
.Lmt_reverse:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    pushl $0                    # [ebp-16] i
.Lmt_rv_loop:
    movl -16(%ebp), %eax        # i
    addl %eax, %eax             # 2*i
    movl -8(%ebp), %ecx         # len
    decl %ecx                   # len-1
    cmpl %ecx, %eax
    jge .Lmt_rv_done
    # a = items+i*8
    movl -12(%ebp), %edx
    movl %eax, %esi
    shll $3, %esi
    addl %esi, %edx             # &items[i]
    # b = items+(len-1-i)*8
    movl -8(%ebp), %eax
    movl -16(%ebp), %ecx
    movl %eax, %edi
    subl %ecx, %edi             # len-i
    decl %edi                   # len-1-i
    shll $3, %edi
    addl -12(%ebp), %edi        # &items[len-1-i]
    movl (%edx), %eax
    xchgl %eax, (%edi)
    movl %eax, (%edx)
    movl 4(%edx), %eax
    xchgl %eax, 4(%edi)
    movl %eax, 4(%edx)
    incl -16(%ebp)
    jmp .Lmt_rv_loop
.Lmt_rv_done:
    jmp .Lmt_none_pop

# ---------------- str (T_STR) ----------------------------------------------
.Lmt_str:
    movl 16(%ebp), %esi
    movl $mn_upper, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_upper
    movl 16(%ebp), %esi
    movl $mn_lower, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_lower
    movl 16(%ebp), %esi
    movl $mn_startswith, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_startswith
    movl 16(%ebp), %esi
    movl $mn_endswith, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_endswith
    movl 16(%ebp), %esi
    movl $mn_find, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_find
    movl 16(%ebp), %esi
    movl $mn_count, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_strcount
    movl 16(%ebp), %esi
    movl $mn_join, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_strjoin
    jmp .Lmt_none_pop

# upper()/lower(): n==0 -> .Lstr_case(obj, 1/0)
.Lmt_upper:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %esi
    movl $1, %edx
    jmp .Lmt_case_go
.Lmt_lower:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %esi
    xorl %edx, %edx
.Lmt_case_go:
    call .Lstr_case
    jmp .Lmt_out

# .Lstr_case: esi=str obj, edx=0 lower / 1 upper -> aval (ASCII only)
.Lstr_case:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] obj
    pushl %edx                  # [ebp-8] mode
    movl 4(%esi), %ebx
    movl %ebx, %eax
    addl $17, %eax
    movl %eax, %ebx
    call .Lalloc
    testl %eax, %eax
    jz .Lscc_fail
    movl %eax, %edi             # new obj
    movl $1, (%edi)
    movl -4(%ebp), %eax
    movl 4(%eax), %ecx
    movl %ecx, 4(%edi)
    incl %ecx
    movl %ecx, 8(%edi)
    movl $0, 12(%edi)
    movl -4(%ebp), %eax
    addl $16, %eax              # src
    movl %edi, %ecx
    addl $16, %ecx              # dst
    movl -4(%ebp), %edx
    movl 4(%edx), %ebx          # count
.Lscc_loop:
    testl %ebx, %ebx
    jz .Lscc_done
    movb (%eax), %dl
    movl -8(%ebp), %esi         # mode
    testl %esi, %esi
    jz .Lscc_lower
    cmpb $0x61, %dl             # 'a'
    jb .Lscc_store
    cmpb $0x7a, %dl             # 'z'
    ja .Lscc_store
    subl $0x20, %edx
    jmp .Lscc_store
.Lscc_lower:
    cmpb $0x41, %dl             # 'A'
    jb .Lscc_store
    cmpb $0x5a, %dl             # 'Z'
    ja .Lscc_store
    addl $0x20, %edx
.Lscc_store:
    movb %dl, (%ecx)
    incl %eax
    incl %ecx
    decl %ebx
    jmp .Lscc_loop
.Lscc_done:
    movb $0, (%ecx)
    movl %edi, %eax
    movl $0xFFFF0000, %edx
    movl %ebp, %esp
    popl %ebp
    ret
.Lscc_fail:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    movl %ebp, %esp
    popl %ebp
    ret

# startswith(prefix)/endswith(suffix): n==1, argv[0] str-ish -> bool
.Lmt_startswith:
    movl $0, %edx               # mode 0 = prefix
    jmp .Lmt_affix_go
.Lmt_endswith:
    movl $1, %edx               # mode 1 = suffix
.Lmt_affix_go:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    pushl %edx                  # [ebp-8] mode
    # needle = argv[0] -> obj ptr or fail
    movl 36(%ebp), %ecx
    movl 4(%ecx), %edx
    movl (%ecx), %eax
    call .Lobjptr
    testl %esi, %esi
    jz .Lmt_affix_bad
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Lmt_affix_nok
    cmpl $12, %ecx
    je .Lmt_affix_nok
    jmp .Lmt_affix_bad
.Lmt_affix_nok:
    # esi = needle obj ; edx = mode on stack ; hay = -4(%ebp)
    movl -4(%ebp), %eax         # hay obj
    movl %eax, %ebx             # save hay (ebx free here)
    # lengths
    movl 4(%esi), %ecx          # nlen
    movl 4(%ebx), %edx          # hlen
    cmpl %edx, %ecx
    ja .Lmt_affix_false         # needle longer -> false
    movl -8(%ebp), %eax         # mode
    testl %eax, %eax
    jz .Lmt_affix_pre
    # suffix: hayoff = hlen - nlen
    movl %edx, %eax
    subl %ecx, %eax
    jmp .Lmt_affix_off
.Lmt_affix_pre:
    xorl %eax, %eax             # offset 0
.Lmt_affix_off:
    # compare nlen bytes: hay+16+off vs needle+16
    movl %ebx, %edx
    addl $16, %edx
    addl %eax, %edx             # hcur
    movl %esi, %edi
    addl $16, %edi              # ncur
    # count in ecx? ecx=nlen still? clobbered? .Lstreq not used; ecx intact
    # but we used ecx for nlen earlier — still valid
.Lmt_affix_cmp:
    testl %ecx, %ecx
    jz .Lmt_affix_true
    movb (%edx), %al
    cmpb %al, (%edi)
    jne .Lmt_affix_false
    incl %edx
    incl %edi
    decl %ecx
    jmp .Lmt_affix_cmp
.Lmt_affix_true:
    movl $6, %eax
    movl $0xFFFF0000, %edx
    jmp .Lmt_out
.Lmt_affix_false:
    movl $4, %eax
    movl $0xFFFF0000, %edx
    jmp .Lmt_out
.Lmt_affix_bad:
    jmp .Lmt_badargs

# find(sub): n==1 -> int index or -1
.Lmt_find:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    # needle = argv[0] str-ish
    movl 36(%ebp), %ecx
    movl 4(%ecx), %edx
    movl (%ecx), %eax
    call .Lobjptr
    testl %esi, %esi
    jz .Lmt_badargs
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Lmt_find_nok
    cmpl $12, %ecx
    je .Lmt_find_nok
    jmp .Lmt_badargs
.Lmt_find_nok:
    movl %esi, %ebx             # needle obj
    movl -4(%ebp), %esi         # hay obj
    # .Lstr_find(hay esi, needle ebx) -> eax = s32 idx or -1
    call .Lstr_find
    testl %eax, %eax
    jns .Lmt_find_pos
    # -1
    movl $0xFFFFFFFF, %esi
    movl $0xFFFFFFFF, %edi
    call .Lrebox
    jmp .Lmt_out
.Lmt_find_pos:
    call .Lmksmall
    jmp .Lmt_out

# .Lstr_find: esi=hay str obj, ebx=needle str obj -> eax = first index or -1.
# Byte search (str subset is byte-oriented). clobbers eax,ecx,edx,esi,edi; ebx preserved? NO - reload carefully: uses ebx as needle, preserves it.
.Lstr_find:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] hay
    pushl %ebx                  # [ebp-8] needle
    movl 4(%ebx), %eax          # nlen
    movl -4(%ebp), %ecx
    movl 4(%ecx), %edx          # hlen
    cmpl %edx, %eax
    ja .Lsf_retneg
    testl %eax, %eax
    jz .Lsf_ret0                # empty needle -> index 0
    # pos range: 0 .. hlen-nlen
    movl %edx, %ecx
    subl %eax, %ecx             # maxpos
    pushl $0                    # [ebp-12] pos
    pushl %ecx                  # [ebp-16] maxpos
.Lsf_pl:
    movl -12(%ebp), %eax
    cmpl -16(%ebp), %eax
    ja .Lsf_retneg
    # hcur = hay+16+pos ; ncur = needle+16 ; cnt=nlen
    movl -4(%ebp), %edx
    addl $16, %edx
    movl -12(%ebp), %ecx
    addl %ecx, %edx             # hcur
    movl -8(%ebp), %edi
    addl $16, %edi              # ncur
    movl -8(%ebp), %ecx
    movl 4(%ecx), %esi          # cnt = nlen
.Lsf_cmp:
    testl %esi, %esi
    jz .Lsf_hit
    movb (%edx), %al
    cmpb %al, (%edi)
    jne .Lsf_pnext
    incl %edx
    incl %edi
    decl %esi
    jmp .Lsf_cmp
.Lsf_pnext:
    incl -12(%ebp)
    jmp .Lsf_pl
.Lsf_hit:
    movl -12(%ebp), %eax
    jmp .Lsf_out
.Lsf_ret0:
    xorl %eax, %eax
    jmp .Lsf_out
.Lsf_retneg:
    movl $-1, %eax
.Lsf_out:
    movl %ebp, %esp
    popl %ebp
    ret

# str count(sub): n==1 -> int count of non-overlapping occurrences
.Lmt_strcount:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl 36(%ebp), %ecx
    movl 4(%ecx), %edx
    movl (%ecx), %eax
    call .Lobjptr
    testl %esi, %esi
    jz .Lmt_badargs
    movl (%esi), %ecx
    cmpl $1, %ecx
    je .Lmt_sc_nok
    cmpl $12, %ecx
    je .Lmt_sc_nok
    jmp .Lmt_badargs
.Lmt_sc_nok:
    movl %esi, %ebx             # needle
    movl -4(%ebp), %esi         # hay
    call .Lstr_count
    call .Lmksmall
    jmp .Lmt_out

# .Lstr_count: esi=hay, ebx=needle -> eax = occurrence count (s32)
.Lstr_count:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi                  # [ebp-4] hay
    pushl %ebx                  # [ebp-8] needle
    movl -8(%ebp), %eax
    movl 4(%eax), %ecx          # nlen
    testl %ecx, %ecx
    jnz .Lsc_nz
    # empty needle: hlen+1
    movl -4(%ebp), %eax
    movl 4(%eax), %eax
    incl %eax
    jmp .Lsc_out
.Lsc_nz:
    movl -4(%ebp), %eax
    movl 4(%eax), %edx          # hlen
    pushl %edx                  # [ebp-12] hlen (edx gets reused as hcur)
    pushl $0                    # [ebp-16] pos
    pushl $0                    # [ebp-20] count
.Lsct_loop:
    movl -16(%ebp), %eax        # pos
    cmpl -12(%ebp), %eax
    ja .Lsct_done               # pos > hlen -> stop
    # remaining = hlen - pos ; if remaining < nlen -> done
    movl -12(%ebp), %ecx
    subl %eax, %ecx             # remaining
    movl -8(%ebp), %edi
    movl 4(%edi), %esi          # nlen
    cmpl %esi, %ecx
    jl .Lsct_done
    # compare at hay+16+pos
    movl -4(%ebp), %edx
    addl $16, %edx
    addl %eax, %edx             # hcur
    movl -8(%ebp), %edi
    addl $16, %edi              # ncur
.Lsccc_cmp:
    testl %esi, %esi
    jz .Lsccc_hit
    movb (%edx), %al
    cmpb %al, (%edi)
    jne .Lsccc_next
    incl %edx
    incl %edi
    decl %esi
    jmp .Lsccc_cmp
.Lsccc_hit:
    incl -20(%ebp)              # count++
    # pos += nlen
    movl -8(%ebp), %eax
    movl 4(%eax), %ecx
    addl %ecx, -16(%ebp)
    jmp .Lsct_loop
.Lsccc_next:
    incl -16(%ebp)              # pos++
    jmp .Lsct_loop
.Lsct_done:
    movl -20(%ebp), %eax
.Lsc_out:
    movl %ebp, %esp
    popl %ebp
    ret

# str join(parts): n==1, argv[0] list/tuple -> a_str_joinsep(obj, len, items)
.Lmt_strjoin:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl 36(%ebp), %eax         # &argv[0]
    movl (%eax), %ecx
    movl 4(%eax), %edx
    call .Lobjptr
    testl %esi, %esi
    jz .Lmt_badargs
    movl (%esi), %ecx
    cmpl $2, %ecx
    je .Lmt_strjoin_ok
    cmpl $3, %ecx
    je .Lmt_strjoin_ok
    jmp .Lmt_badargs
.Lmt_strjoin_ok:
    movl 4(%esi), %edx          # n = parts len
    movl 16(%esi), %ecx         # items
    pushl %ecx                  # items
    pushl $0                    # n hi
    pushl %edx                  # n lo
    pushl $0xFFFF0000           # sep hi (obj aval hi)
    movl -4(%ebp), %eax         # sep lo = obj ptr
    pushl %eax
    call a_str_joinsep
    addl $20, %esp
    jmp .Lmt_out

# ---------------- dict (T_DICT) ---------------------------------------------
.Lmt_dict:
    movl 16(%ebp), %esi
    movl $mn_get, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_dget
    movl 16(%ebp), %esi
    movl $mn_contains, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_dcontains
    movl 16(%ebp), %esi
    movl $mn_clear, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_dclear
    jmp .Lmt_none_pop

# get(k[, dflt]): n==1/2 -> v or dflt/none
.Lmt_dget:
    movl 20(%ebp), %eax
    testl %eax, %eax
    jz .Lmt_badargs
    cmpl $2, %eax
    ja .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %esi         # dict
    movl 36(%ebp), %edi         # &argv[0] = key
    call .Ldict_find
    testl %eax, %eax
    jns .Lmt_dgot
    movl 20(%ebp), %eax
    cmpl $2, %eax
    jne .Lmt_none_pop           # no default -> A_NONE
    movl 36(%ebp), %ecx
    movl 8(%ecx), %eax          # argv[1] lo
    movl 12(%ecx), %edx         # argv[1] hi
    jmp .Lmt_out
.Lmt_dgot:
    movl -4(%ebp), %ecx
    movl 16(%ecx), %edx         # pairs
    shll $4, %eax
    addl %eax, %edx
    movl 8(%edx), %eax
    movl 12(%edx), %edx
    jmp .Lmt_out

# contains(k): n==1 -> bool
.Lmt_dcontains:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %esi
    movl 36(%ebp), %edi
    call .Ldict_find
    testl %eax, %eax
    jns .Lmc_true
    jmp .Lmc_false

# clear(): n==0
.Lmt_dclear:
    movl 20(%ebp), %eax
    orl 24(%ebp), %eax
    jnz .Lmt_badargs
    movl -4(%ebp), %eax
    movl $0, 4(%eax)
    jmp .Lmt_none_pop

# ---------------- set (T_SET) -----------------------------------------------
# locals same as seq: -4 obj, -8 len, -12 items
.Lmt_set:
    movl -4(%ebp), %eax
    movl 4(%eax), %ecx
    pushl %ecx                  # [ebp-8] len
    movl 16(%eax), %ecx
    pushl %ecx                  # [ebp-12] items
    movl 16(%ebp), %esi
    movl $mn_contains, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_contains
    movl 16(%ebp), %esi
    movl $mn_add, %edi
    call .Lstreq
    testl %eax, %eax
    jnz .Lmt_sadd
    jmp .Lmt_none_pop

# add(v): n==1; dedup via .Leqkey then append
.Lmt_sadd:
    movl 20(%ebp), %eax
    cmpl $1, %eax
    jne .Lmt_badargs
    movl 24(%ebp), %eax
    testl %eax, %eax
    jnz .Lmt_badargs
    pushl $0                    # [ebp-16] i
.Lmsa_scan:
    movl -16(%ebp), %eax
    cmpl -8(%ebp), %eax
    jae .Lmsa_add
    movl -12(%ebp), %edx        # items
    movl %eax, %ecx
    shll $3, %ecx
    addl %ecx, %edx             # &items[i]
    movl %edx, %esi             # A = item
    movl 36(%ebp), %edi         # B = argv[0]
    call .Leqkey
    testl %eax, %eax
    jnz .Lmt_none_pop           # present -> A_NONE
    incl -16(%ebp)
    jmp .Lmsa_scan
.Lmsa_add:
    movl -4(%ebp), %esi         # set obj
    call .Larr_ensure
    testl %eax, %eax
    jz .Lmt_none_pop
    movl -4(%ebp), %esi
    movl 4(%esi), %ecx
    movl 16(%esi), %edx
    shll $3, %ecx
    addl %ecx, %edx
    movl 36(%ebp), %eax
    movl (%eax), %ecx
    movl %ecx, (%edx)
    movl 4(%eax), %ecx
    movl %ecx, 4(%edx)
    incl 4(%esi)
    jmp .Lmt_none_pop

.Lmt_badargs:
    jmp .Lmt_none_pop
.Lmt_none_pop:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
.Lmt_out:
    movl %ebp, %esp
    popl %ebp
    ret
.Lmt_none:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret


    .globl a_setattr
a_setattr:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_range_contains
a_range_contains:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_test_main
a_test_main:
    xorl %eax, %eax
    xorl %edx, %edx
    ret

# ---- data -----------------------------------------------------------------
    .section .data
    .globl int_scratch
int_scratch:
    .long 0
    .long 0
    .long 0
    .long 0

# a_in_range_inclusive(v,lo,hi): subset — alias to exclusive-bound check
    .section .text
    .globl a_in_range_inclusive
a_in_range_inclusive:
    jmp a_in_range
