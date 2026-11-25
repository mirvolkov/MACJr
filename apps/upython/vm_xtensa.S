/*
 * MicroPython VM fast path in Xtensa LX7 assembly (MACJr, ESP32-S3).
 *
 *   uint64_t macjr_vm_fast(const byte *ip, mp_obj_t *sp, mp_obj_t *fastn,
 *                          volatile mp_obj_t *pending);
 *
 * Called by mp_execute_bytecode (vm.c) at every dispatch. Runs bytecode
 * from ip as long as the opcodes are simple ones it knows, and returns at
 * the first one it doesn't (or can't do here: a float add, a small-int
 * overflow, an unset local...) with ip pointing at that opcode and sp
 * untouched by it, so the C switch runs it as if this never happened.
 * Return value: ip in the low word (a2), sp in the high word (a3).
 *
 * Nothing here calls out, allocates or raises: an opcode is either done
 * completely or not started. A jump is not started while an exception is
 * pending (*pending != NULL), so the C side does the jump and its check.
 *
 * Object representation A (mpconfig.h): small int = (n << 1) | 1,
 * None = 6, False = 14, True = 30. Opcode numbers: py/bc0.h, binary op
 * numbers: py/runtime0.h.
 *
 * Registers (one windowed frame, no calls):
 *   a2 ip     a3 sp     a4 fastn     a5 pending     a6 jump table
 *   a7 opcode a8..a15 scratch
 */

#define NONE   6
#define FALSE  14
#define TRUE   30

/* Fetch the opcode at ip and jump to its handler. */
.macro NEXT
    l8ui    a7, a2, 0
    addx4   a8, a7, a6
    l32i    a8, a8, 0
    jx      a8
.endm

.macro PUSH reg
    addi    a3, a3, 4
    s32i    \reg, a3, 0
.endm

/* Python truth of a small int / bool / None in obj: out = 0 or 1.
 * Anything else (a list, a float...) goes back to C. */
.macro TRUTH obj, out, tmp
    movi    \out, 1
    bbsi    \obj, 0, .Lsi\@
    movi    \tmp, TRUE
    beq     \obj, \tmp, .Lend\@
    movi    \out, 0
    movi    \tmp, FALSE
    beq     \obj, \tmp, .Lend\@
    movi    \tmp, NONE
    beq     \obj, \tmp, .Lend\@
    j       .Lbail
.Lsi\@:
    bnei    \obj, 1, .Lend\@        /* small int: true unless 0 (word 1) */
    movi    \out, 0
.Lend\@:
.endm

/* Unsigned var-int at p (advanced past it) -> out. */
.macro DECODE_UINT p, out, t, t2
    movi    \out, 0
.Ldu\@:
    l8ui    \t, \p, 0
    addi    \p, \p, 1
    slli    \out, \out, 7
    extui   \t2, \t, 0, 7
    or      \out, \out, \t2
    bbsi    \t, 7, .Ldu\@
.endm

/* Signed jump offset at ip+1 -> off; ip after the operand -> p. */
.macro DECODE_SLABEL off, p, t
    l8ui    \off, a2, 1
    addi    \p, a2, 2
    bbsi    \off, 7, .Lsl2\@
    addi    \off, \off, -0x40
    j       .Lsle\@
.Lsl2\@:
    l8ui    \t, a2, 2
    extui   \off, \off, 0, 7
    slli    \t, \t, 7
    or      \off, \off, \t
    addmi   \off, \off, -0x4000
    addi    \p, \p, 1
.Lsle\@:
.endm

/* Unsigned jump offset at ip+1 -> off; ip after the operand -> p. */
.macro DECODE_ULABEL off, p, t
    l8ui    \off, a2, 1
    addi    \p, a2, 2
    bbci    \off, 7, .Lule\@
    l8ui    \t, a2, 2
    extui   \off, \off, 0, 7
    slli    \t, \t, 7
    or      \off, \off, \t
    addi    \p, \p, 1
.Lule\@:
.endm

/* Bail out of a jump while an exception is pending. */
.macro PENDING_CHECK
    l32i    a8, a5, 0
    bnez    a8, .Lbail
.endm

/* Binary ops: lhs -> a8, rhs -> a9, both small ints or back to C;
 * a14 = sp - 1 (l32i/s32i offsets can't be negative). */
.macro BIN_LOAD
    addi    a14, a3, -4
    l32i    a8, a14, 0
    l32i    a9, a3, 0
.endm

.macro BIN_SMALL
    BIN_LOAD
    and     a10, a8, a9
    bbci    a10, 0, .Lbail
.endm

/* Store the result a10 over lhs, pop rhs, step past the opcode. */
.macro BIN_DONE
    s32i    a10, a14, 0
    mov     a3, a14
    addi    a2, a2, 1
    NEXT
.endm

/* Compare: a10 = True if (a8 cond a9) on tagged words (order-preserving). */
.macro CMP_SMALL branch_false, swap=0
    BIN_SMALL
    movi    a10, FALSE
    .if \swap
    \branch_false a9, a8, .Lcmp\@
    .else
    \branch_false a8, a9, .Lcmp\@
    .endif
    movi    a10, TRUE
.Lcmp\@:
    BIN_DONE
.endm

    .section .text.macjr_vm_fast, "ax", @progbits
    .literal_position
    .align  4
    .global macjr_vm_fast
    .hidden macjr_vm_fast
    .type   macjr_vm_fast, @function
macjr_vm_fast:
    entry   a1, 32
    movi    a6, .Ltable
    NEXT

.Lbail:
    retw                            /* a2 = ip, a3 = sp: already in place */

/* ---------- constants ---------- */

.Lconst_false:                      /* 0x50 */
    movi    a8, FALSE
    j       .Lpush_a8_1
.Lconst_none:                       /* 0x51 */
    movi    a8, NONE
    j       .Lpush_a8_1
.Lconst_true:                       /* 0x52 */
    movi    a8, TRUE
    j       .Lpush_a8_1
.Lload_null:                        /* 0x53 */
    movi    a8, 0
.Lpush_a8_1:
    PUSH    a8
    addi    a2, a2, 1
    NEXT

.Lsmall_int_multi:                  /* 0x70..0xaf: n = op - 0x80 */
    slli    a8, a7, 1
    addi    a8, a8, -255            /* (op - 0x80) * 2 + 1 */
    PUSH    a8
    addi    a2, a2, 1
    NEXT

.Lsmall_int:                        /* 0x22: signed var-int */
    addi    a9, a2, 1
    l8ui    a10, a9, 0
    movi    a8, 0
    bbci    a10, 6, 1f
    movi    a8, -1
1:  l8ui    a10, a9, 0
    addi    a9, a9, 1
    slli    a8, a8, 7
    extui   a11, a10, 0, 7
    or      a8, a8, a11
    bbsi    a10, 7, 1b
    slli    a8, a8, 1
    addi    a8, a8, 1
    PUSH    a8
    mov     a2, a9
    NEXT

/* ---------- locals: fastn[-n] ---------- */

.Lload_fast_multi:                  /* 0xb0..0xbf */
    slli    a8, a7, 2
    sub     a8, a4, a8
    l32i    a9, a8, 0xb0 * 4
    beqz    a9, .Lbail              /* unset: NameError, in C */
    PUSH    a9
    addi    a2, a2, 1
    NEXT

.Lstore_fast_multi:                 /* 0xc0..0xcf */
    slli    a8, a7, 2
    sub     a8, a4, a8
    l32i    a9, a3, 0
    addi    a3, a3, -4
    s32i    a9, a8, 0xc0 * 4
    addi    a2, a2, 1
    NEXT

.Lload_fast_n:                      /* 0x24 */
    addi    a9, a2, 1
    DECODE_UINT a9, a10, a11, a12
    slli    a10, a10, 2
    sub     a10, a4, a10
    l32i    a8, a10, 0
    beqz    a8, .Lbail
    PUSH    a8
    mov     a2, a9
    NEXT

.Lstore_fast_n:                     /* 0x26 */
    addi    a9, a2, 1
    DECODE_UINT a9, a10, a11, a12
    slli    a10, a10, 2
    sub     a10, a4, a10
    l32i    a8, a3, 0
    addi    a3, a3, -4
    s32i    a8, a10, 0
    mov     a2, a9
    NEXT

/* ---------- stack ---------- */

.Ldup_top:                          /* 0x57 */
    l32i    a8, a3, 0
    PUSH    a8
    addi    a2, a2, 1
    NEXT

.Ldup_top_two:                      /* 0x58 */
    addi    a10, a3, -4
    l32i    a8, a10, 0
    l32i    a9, a3, 0
    s32i    a8, a3, 4
    s32i    a9, a3, 8
    addi    a3, a3, 8
    addi    a2, a2, 1
    NEXT

.Lpop_top:                          /* 0x59 */
    addi    a3, a3, -4
    addi    a2, a2, 1
    NEXT

.Lrot_two:                          /* 0x5a */
    addi    a10, a3, -4
    l32i    a8, a3, 0
    l32i    a9, a10, 0
    s32i    a9, a3, 0
    s32i    a8, a10, 0
    addi    a2, a2, 1
    NEXT

.Lrot_three:                        /* 0x5b */
    addi    a11, a3, -8
    l32i    a8, a3, 0               /* top */
    l32i    a9, a11, 4              /* sp[-1] */
    l32i    a10, a11, 0             /* sp[-2] */
    s32i    a9, a3, 0
    s32i    a10, a11, 4
    s32i    a8, a11, 0
    addi    a2, a2, 1
    NEXT

/* ---------- jumps ---------- */

.Ljump:                             /* 0x42 */
    PENDING_CHECK
    DECODE_SLABEL a8, a9, a10
    add     a2, a9, a8
    NEXT

.Lpop_jump_if_true:                 /* 0x43 */
    PENDING_CHECK
    l32i    a10, a3, 0
    TRUTH   a10, a11, a12
    DECODE_SLABEL a8, a9, a10
    addi    a3, a3, -4
    mov     a2, a9
    beqz    a11, 1f
    add     a2, a9, a8
1:  NEXT

.Lpop_jump_if_false:                /* 0x44 */
    PENDING_CHECK
    l32i    a10, a3, 0
    TRUTH   a10, a11, a12
    DECODE_SLABEL a8, a9, a10
    addi    a3, a3, -4
    mov     a2, a9
    bnez    a11, 1f
    add     a2, a9, a8
1:  NEXT

.Ljump_if_true_or_pop:              /* 0x45 */
    PENDING_CHECK
    l32i    a10, a3, 0
    TRUTH   a10, a11, a12
    DECODE_ULABEL a8, a9, a10
    mov     a2, a9
    beqz    a11, 1f
    add     a2, a9, a8              /* true: jump, keep the value */
    NEXT
1:  addi    a3, a3, -4              /* false: pop, fall through */
    NEXT

.Ljump_if_false_or_pop:             /* 0x46 */
    PENDING_CHECK
    l32i    a10, a3, 0
    TRUTH   a10, a11, a12
    DECODE_ULABEL a8, a9, a10
    mov     a2, a9
    bnez    a11, 1f
    add     a2, a9, a8              /* false: jump, keep the value */
    NEXT
1:  addi    a3, a3, -4
    NEXT

/* ---------- unary ops ---------- */

.Lunary_positive:                   /* 0xd0 */
    l32i    a8, a3, 0
    bbci    a8, 0, .Lbail
    addi    a2, a2, 1
    NEXT

.Lunary_negative:                   /* 0xd1: -(2x+1) + 2 = 2(-x)+1 */
    l32i    a8, a3, 0
    bbci    a8, 0, .Lbail
    movi    a9, 2
    sub     a9, a9, a8
    bne     a9, a8, 1f              /* equal only for 0 and the minimum, */
    bnei    a8, 1, .Lbail           /* whose negation doesn't fit */
1:  s32i    a9, a3, 0
    addi    a2, a2, 1
    NEXT

.Lunary_invert:                     /* 0xd2: ~x = 2(-x-1)+1 = a ^ ~1 */
    l32i    a8, a3, 0
    bbci    a8, 0, .Lbail
    movi    a9, -2
    xor     a8, a8, a9
    s32i    a8, a3, 0
    addi    a2, a2, 1
    NEXT

.Lunary_not:                        /* 0xd3 */
    l32i    a8, a3, 0
    TRUTH   a8, a9, a10
    movi    a10, TRUE
    beqz    a9, 1f
    movi    a10, FALSE
1:  s32i    a10, a3, 0
    addi    a2, a2, 1
    NEXT

/* ---------- binary ops ---------- */

.Lbin_less:     CMP_SMALL bge               /* 0xd7  a <  b */
.Lbin_more:     CMP_SMALL bge, 1            /* 0xd8  a >  b: false if b >= a */
.Lbin_equal:    CMP_SMALL bne               /* 0xd9 */
.Lbin_le:       CMP_SMALL blt, 1            /* 0xda  a <= b: false if b < a */
.Lbin_ge:       CMP_SMALL blt               /* 0xdb  a >= b: false if a < b */
.Lbin_ne:       CMP_SMALL beq               /* 0xdc */

.Lbin_is:                           /* 0xde: any objects */
    BIN_LOAD
    movi    a10, FALSE
    bne     a8, a9, 1f
    movi    a10, TRUE
1:  BIN_DONE

.Lbin_or:                           /* 0xe0, 0xed */
    BIN_SMALL
    or      a10, a8, a9
    BIN_DONE

.Lbin_and:                          /* 0xe2, 0xef */
    BIN_SMALL
    and     a10, a8, a9
    BIN_DONE

.Lbin_xor:                          /* 0xe1, 0xee */
    BIN_SMALL
    xor     a10, a8, a9
    addi    a10, a10, 1
    BIN_DONE

.Lbin_add:                          /* 0xe5, 0xf2: (2x+1) + 2y */
    BIN_SMALL
    addi    a11, a9, -1
    add     a10, a8, a11
    xor     a12, a10, a8
    xor     a13, a10, a11
    and     a12, a12, a13
    bltz    a12, .Lbail             /* overflow: a big int, in C */
    BIN_DONE

.Lbin_sub:                          /* 0xe6, 0xf3: (2x+1) - 2y */
    BIN_SMALL
    addi    a11, a9, -1
    sub     a10, a8, a11
    xor     a12, a8, a11
    xor     a13, a8, a10
    and     a12, a12, a13
    bltz    a12, .Lbail
    BIN_DONE

.Lbin_mul:                          /* 0xe7, 0xf4 */
    BIN_SMALL
    srai    a8, a8, 1
    srai    a9, a9, 1
    mull    a10, a8, a9
    mulsh   a11, a8, a9
    srai    a12, a10, 31
    bne     a11, a12, .Lbail        /* doesn't fit 32 bits */
    srai    a13, a10, 30
    bne     a13, a12, .Lbail        /* doesn't fit 31 bits */
    slli    a10, a10, 1
    addi    a10, a10, 1
    BIN_DONE

.Lbin_floordiv:                     /* 0xe9, 0xf6 */
    BIN_SMALL
    srai    a8, a8, 1
    srai    a9, a9, 1
    beqz    a9, .Lbail              /* ZeroDivisionError, in C */
    quos    a10, a8, a9
    rems    a11, a8, a9
    beqz    a11, 1f
    xor     a12, a11, a9
    bgez    a12, 1f
    addi    a10, a10, -1            /* round toward minus infinity */
1:  slli    a12, a10, 1
    xor     a12, a12, a10
    bltz    a12, .Lbail             /* minimum // -1 doesn't fit */
    slli    a10, a10, 1
    addi    a10, a10, 1
    BIN_DONE

.Lbin_mod:                          /* 0xeb, 0xf8 */
    BIN_SMALL
    srai    a8, a8, 1
    srai    a9, a9, 1
    beqz    a9, .Lbail
    rems    a10, a8, a9
    beqz    a10, 1f
    xor     a12, a10, a9
    bgez    a12, 1f
    add     a10, a10, a9            /* result takes the divisor's sign */
1:  slli    a10, a10, 1
    addi    a10, a10, 1
    BIN_DONE

.Lbin_lshift:                       /* 0xe3, 0xf0 */
    BIN_SMALL
    srai    a9, a9, 1
    bltz    a9, .Lbail              /* ValueError, in C */
    movi    a11, 31
    bge     a9, a11, .Lbail
    addi    a8, a8, -1              /* 2x */
    ssl     a9
    sll     a10, a8
    ssr     a9
    sra     a11, a10
    bne     a11, a8, .Lbail         /* bits lost: a big int, in C */
    addi    a10, a10, 1
    BIN_DONE

.Lbin_rshift:                       /* 0xe4, 0xf1 */
    BIN_SMALL
    srai    a9, a9, 1
    bltz    a9, .Lbail
    movi    a11, 31
    blt     a9, a11, 1f
    mov     a9, a11                 /* big shifts: just the sign */
1:  srai    a8, a8, 1
    ssr     a9
    sra     a10, a8
    slli    a10, a10, 1
    addi    a10, a10, 1
    BIN_DONE

    .size   macjr_vm_fast, . - macjr_vm_fast

/* ---------- opcode -> handler ---------- */

.macro ENT n
    .if (\n >= 0x70) && (\n < 0xb0)
    .word   .Lsmall_int_multi
    .elseif (\n >= 0xb0) && (\n < 0xc0)
    .word   .Lload_fast_multi
    .elseif (\n >= 0xc0) && (\n < 0xd0)
    .word   .Lstore_fast_multi
    .elseif \n == 0x22
    .word   .Lsmall_int
    .elseif \n == 0x24
    .word   .Lload_fast_n
    .elseif \n == 0x26
    .word   .Lstore_fast_n
    .elseif \n == 0x42
    .word   .Ljump
    .elseif \n == 0x43
    .word   .Lpop_jump_if_true
    .elseif \n == 0x44
    .word   .Lpop_jump_if_false
    .elseif \n == 0x45
    .word   .Ljump_if_true_or_pop
    .elseif \n == 0x46
    .word   .Ljump_if_false_or_pop
    .elseif \n == 0x50
    .word   .Lconst_false
    .elseif \n == 0x51
    .word   .Lconst_none
    .elseif \n == 0x52
    .word   .Lconst_true
    .elseif \n == 0x53
    .word   .Lload_null
    .elseif \n == 0x57
    .word   .Ldup_top
    .elseif \n == 0x58
    .word   .Ldup_top_two
    .elseif \n == 0x59
    .word   .Lpop_top
    .elseif \n == 0x5a
    .word   .Lrot_two
    .elseif \n == 0x5b
    .word   .Lrot_three
    .elseif \n == 0xd0
    .word   .Lunary_positive
    .elseif \n == 0xd1
    .word   .Lunary_negative
    .elseif \n == 0xd2
    .word   .Lunary_invert
    .elseif \n == 0xd3
    .word   .Lunary_not
    .elseif \n == 0xd7
    .word   .Lbin_less
    .elseif \n == 0xd8
    .word   .Lbin_more
    .elseif \n == 0xd9
    .word   .Lbin_equal
    .elseif \n == 0xda
    .word   .Lbin_le
    .elseif \n == 0xdb
    .word   .Lbin_ge
    .elseif \n == 0xdc
    .word   .Lbin_ne
    .elseif \n == 0xde
    .word   .Lbin_is
    .elseif (\n == 0xe0) || (\n == 0xed)
    .word   .Lbin_or
    .elseif (\n == 0xe1) || (\n == 0xee)
    .word   .Lbin_xor
    .elseif (\n == 0xe2) || (\n == 0xef)
    .word   .Lbin_and
    .elseif (\n == 0xe3) || (\n == 0xf0)
    .word   .Lbin_lshift
    .elseif (\n == 0xe4) || (\n == 0xf1)
    .word   .Lbin_rshift
    .elseif (\n == 0xe5) || (\n == 0xf2)
    .word   .Lbin_add
    .elseif (\n == 0xe6) || (\n == 0xf3)
    .word   .Lbin_sub
    .elseif (\n == 0xe7) || (\n == 0xf4)
    .word   .Lbin_mul
    .elseif (\n == 0xe9) || (\n == 0xf6)
    .word   .Lbin_floordiv
    .elseif (\n == 0xeb) || (\n == 0xf8)
    .word   .Lbin_mod
    .else
    .word   .Lbail
    .endif
.endm

    /* absolute addresses, relocated at load: .data.rel.ro, not .rodata */
    .section .data.rel.ro.macjr_vm_table, "aw"
    .align  4
.Ltable:
    .set    op, 0
    .rept   256
    ENT     op
    .set    op, op + 1
    .endr
