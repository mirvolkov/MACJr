    .section .text
    .align  4

# -----------------------------------------------------------------
# int32_t dot_i32_asm(const int32_t *a, const int32_t *b, int32_t n)
#   returns sum of a[i]*b[i] for i in [0,n)
#
# args: a2=a, a3=b, a4=n   return: a2
# -----------------------------------------------------------------
    .align  4
    .global dot_i32_asm
    .type   dot_i32_asm,@function
dot_i32_asm:
    entry   a1, 16
    movi    a5, 0                # acc = 0
    beqz    a4, .Ldot_ret
.Ldot_loop:
    l32i    a6, a2, 0
    l32i    a7, a3, 0
    mull    a6, a6, a7
    add     a5, a5, a6
    addi    a2, a2, 4
    addi    a3, a3, 4
    addi    a4, a4, -1
    bnez    a4, .Ldot_loop
.Ldot_ret:
    mov     a2, a5
    retw
    .size   dot_i32_asm, . - dot_i32_asm

# -----------------------------------------------------------------
# void matvec_i32_asm(int32_t *y, const int32_t *W,
#                     const int32_t *x, const int32_t *b,
#                     int32_t rows, int32_t cols)
#   y[rows] = W[rows x cols] * x[cols] + b[rows]   (b may be NULL)
#
# args: a2=y, a3=W, a4=x, a5=b, a6=rows, a7=cols
# -----------------------------------------------------------------
    .align  4
    .global matvec_i32_asm
    .type   matvec_i32_asm,@function
matvec_i32_asm:
    entry   a1, 48
    beqz    a6, .Lmv_ret
    slli    a15, a7, 2           # row_stride = cols * 4
    movi    a8, 0                # i = 0
.Lmv_row:
    beq     a8, a6, .Lmv_ret
    movi    a9, 0                # acc = 0
    movi    a10, 0               # k = 0
    mull    a11, a8, a15
    add     a11, a3, a11         # W_row = &W[i*cols]
    mov     a12, a4              # x_ptr = x
.Lmv_col:
    beq     a10, a7, .Lmv_col_done
    l32i    a13, a11, 0
    l32i    a14, a12, 0
    mull    a13, a13, a14
    add     a9, a9, a13
    addi    a11, a11, 4
    addi    a12, a12, 4
    addi    a10, a10, 1
    j       .Lmv_col
.Lmv_col_done:
    beqz    a5, .Lmv_store
    slli    a10, a8, 2
    add     a10, a5, a10
    l32i    a13, a10, 0          # b[i]
    add     a9, a9, a13
.Lmv_store:
    slli    a10, a8, 2
    add     a10, a2, a10
    s32i    a9, a10, 0
    addi    a8, a8, 1
    j       .Lmv_row
.Lmv_ret:
    retw
    .size   matvec_i32_asm, . - matvec_i32_asm

# -----------------------------------------------------------------
# void matmul_i32_asm(int32_t *C, const int32_t *A, const int32_t *B, int32_t n)
#   C[n x n] = A[n x n] * B[n x n]     (row-major, int32)
#
# args (windowed ABI): a2=C, a3=A, a4=B, a5=n
# -----------------------------------------------------------------
    .align  4
    .global matmul_i32_asm
    .type   matmul_i32_asm,@function
matmul_i32_asm:
    entry   a1, 32

    beqz    a5, .Lret            # n == 0

    slli    a15, a5, 2           # a15 = n*4 (row stride, bytes)

    movi    a6, 0                # i = 0
.Li_loop:
    beq     a6, a5, .Lret

    movi    a7, 0                # j = 0
.Lj_loop:
    beq     a7, a5, .Li_next

    movi    a9, 0                # sum = 0
    movi    a8, 0                # k = 0

    mull    a10, a6, a15         # a10 = i*n*4
    add     a10, a3, a10         # a10 = &A[i*n]        (step +4 by k)

    slli    a11, a7, 2
    add     a11, a4, a11         # a11 = &B[j]          (step +n*4 by k)

.Lk_loop:
    beq     a8, a5, .Lk_done

    l32i    a12, a10, 0          # A[i*n+k]
    l32i    a13, a11, 0          # B[k*n+j]
    mull    a12, a12, a13
    add     a9, a9, a12

    addi    a10, a10, 4
    add     a11, a11, a15
    addi    a8, a8, 1
    j       .Lk_loop

.Lk_done:
    mull    a12, a6, a15
    add     a12, a2, a12
    slli    a13, a7, 2
    add     a12, a12, a13
    s32i    a9, a12, 0           # C[i*n+j] = sum

    addi    a7, a7, 1
    j       .Lj_loop

.Li_next:
    addi    a6, a6, 1
    j       .Li_loop

.Lret:
    retw

    .size   matmul_i32_asm, . - matmul_i32_asm
