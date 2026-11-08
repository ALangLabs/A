# i8086 bare-metal runtime subset for the native A backend.
# aval = NaN-boxed uint64: int = 0xFFFF000000000000 | ((i<<1)|1), i is
# a signed 47-bit value stored in the low 48 payload bits.
# cdecl: ret@0(%ebp+4), arg1@8 (8 bytes), arg2@16, arg3@24 ...
# returns: eax=lo32 edx=hi32 of aval.

    .section .text
    .globl a_runtime_init
a_runtime_init:
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

# a_truthy_raw_slow(aval a) -> aval
    .globl a_truthy_raw_slow
a_truthy_raw_slow:
    pushl %ebp
    movl %esp, %ebp
    movl 8(%ebp), %eax
    orl 12(%ebp), %eax
    jz .Lt_false
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
.Lt_false:
    movl $0x00000004, %eax
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

# a_bi_call(id, n, argv) -> aval ; id 0 = print
    .globl a_bi_call
a_bi_call:
    pushl %ebp
    movl %esp, %ebp
    cmpl $0, 8(%ebp)          # id low32 == 0 (print)?
    jne .Lb_none
.Lb_args:
    xorl %ecx, %ecx          # i = 0
.Lb_loop:
    cmpl 16(%ebp), %ecx       # n (8-byte arg at +16)
    jge .Lb_nl
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
    jz .Lb_q
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
.Lb_nl:
    movb $10, %al
    call .Lputc
    movl $0x00000006, %eax
    movl $0xFFFF0000, %edx
    popl %ebp
    ret
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

.Lputc:                     # al -> COM1; preserves edx (callers keep i_lo there)
    pushl %edx
    movw $0x3F8, %dx
    outb %al, %dx
    popl %edx
    ret

    .section .data
    .globl serial_init
serial_init: .byte 0

# ---- stubs: unimplemented builtins return A_NONE ------------------------
# A_NONE = 0xFFFF000000000002
    .globl a_dict_new
a_dict_new:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_str_new
a_str_new:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_dict_set
a_dict_set:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_getattr
a_getattr:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_list_new
a_list_new:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_not
a_not:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_call
a_call:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_method
a_method:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_le_slow
a_le_slow:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_getitem
a_getitem:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_gt_slow
a_gt_slow:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_iter_new
a_iter_new:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_iter_next
a_iter_next:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
    .globl a_fnon_new
a_fnon_new:
    movl $0x00000002, %eax
    movl $0xFFFF0000, %edx
    ret
