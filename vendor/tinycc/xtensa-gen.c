/*
 *  Xtensa code generator for TCC: ESP32-S3 (LX7), windowed ABI with CALL8,
 *  as the ESP-IDF firmware is built. Written for MACJr.
 *
 *  Registers. TCC allocates a10..a15 (also the outgoing argument and the
 *  return registers: CALL8 rotates the window by 8, so the callee sees
 *  them as a2..a7) and a2..a6. Fixed roles:
 *    a0  return address          a1  stack pointer
 *    a7  frame base (frame top - FP_BIAS), so locals reach with one L32I
 *    a8  scratch for addresses and literals (clobbered by calls anyway)
 *    a9  carry from TOK_ADDC1/SUBC1 to TOK_ADDC2/SUBC2
 *  Incoming arguments arrive in a2..a7 and are stored to the frame in the
 *  prologue, so a2..a6 are free from there on.
 *
 *  Frame (GCC's layout): sp -> outgoing stack arguments, locals, 32 bytes
 *  the window ABI reserves; the caller's outgoing arguments (our incoming
 *  stack arguments) start right above, at frame top + 32. The frame size
 *  is known only at the end of the function, so the prologue is written
 *  last, into PROLOG_SIZE bytes kept free at the start.
 *
 *  Addresses. The program is loaded section by section (code and data in
 *  different memory), so nothing is PC-relative: every symbol address is
 *  an inline literal word with an R_XTENSA_32 relocation, jumped over and
 *  read with L32R. Calls are L32R a8 + CALLX8 a8.
 *
 *  Variadic functions: va_list is GCC's {__va_stk, __va_reg, __va_ndx}
 *  (see stdarg.h), so one can be handed to the firmware's vsnprintf(). The
 *  prologue saves all of a2..a7 in one 24-byte area, the named arguments
 *  included, and va_start() points __va_reg at it.
 *
 *  Not supported (yet): floating point, VLAs, inline assembly.
 */

#ifdef TARGET_DEFS_ONLY

#define NB_REGS 12

/* Register classes, from more general to more precise */
#define RC_INT   (1 << 0)
#define RC_FLOAT (1 << 1)
#define RC_R(x)  (1 << (2 + (x))) /* x = 0..5: a10..a15 */

#define RC_IRET RC_R(0) /* a10 */
#define RC_IRE2 RC_R(1) /* a11 */
#define RC_FRET RC_FLOAT

#define REG_IRET 0
#define REG_IRE2 1
#define REG_FRET 11 /* placeholder: there is no float register */

#define PTR_SIZE 4

#define LDOUBLE_SIZE  8
#define LDOUBLE_ALIGN 8
#define MAX_ALIGN     16

#define CHAR_IS_UNSIGNED

/* return values need to be extended at the caller side */
#define PROMOTE_RET

#else /* !TARGET_DEFS_ONLY */

#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char * const target_machine_defs =
    "__xtensa__\0"
    "__XTENSA__\0"
    "__XTENSA_EL__\0"
    "__XTENSA_WINDOWED_ABI__\0"
    "__XTENSA_SOFT_FLOAT__\0"
    "__MACJR__\0"
    ;

ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_R(0), /* a10 */
    RC_INT | RC_R(1), /* a11 */
    RC_INT | RC_R(2), /* a12 */
    RC_INT | RC_R(3), /* a13 */
    RC_INT | RC_R(4), /* a14 */
    RC_INT | RC_R(5), /* a15 */
    RC_INT,           /* a2 */
    RC_INT,           /* a3 */
    RC_INT,           /* a4 */
    RC_INT,           /* a5 */
    RC_INT,           /* a6 */
    RC_FLOAT,         /* placeholder */
};

static const unsigned char phys_reg[NB_REGS - 1] = {10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6};

#define A_SP 1
#define A_FP 7
#define A_T  8
#define A_C  9

#define FP_BIAS     256 /* a7 = frame top - FP_BIAS */
#define ARG_REGS    6
#define PROLOG_SIZE 30  /* entry, 2 x addi, 6 x s32i, mov: all 3 bytes */
#define ZERO_REG    0xff /* in cmp_r: compare against zero */

static int func_prolog_ind, func_max_out, n_arg_save;
static unsigned char arg_save_reg[ARG_REGS];
static int arg_save_loc[ARG_REGS];
static int va_reg_loc, va_ndx; /* variadic: register save area, first __va_ndx */

static void no_float(void)
{
    tcc_error("floating point is not supported yet");
}

static int preg(int r)
{
    if (r < 0 || r >= NB_REGS - 1)
        no_float();
    return phys_reg[r];
}

/* ------------------------------------------------------------------------- */
/* instruction encoding */

static void o3(uint32_t c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 3;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    cur_text_section->data[ind + 1] = c >> 8;
    cur_text_section->data[ind + 2] = c >> 16;
    ind = ind1;
}

ST_FUNC void o(unsigned int c)
{
    o3(c);
}

static uint32_t read24(int pos)
{
    unsigned char *p = cur_text_section->data + pos;
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
}

static void write24(int pos, uint32_t c)
{
    unsigned char *p = cur_text_section->data + pos;
    p[0] = c;
    p[1] = c >> 8;
    p[2] = c >> 16;
}

#define RRR(op2, op1, r, s, t) \
    ((uint32_t)(op2) << 20 | (op1) << 16 | (r) << 12 | (s) << 8 | (t) << 4)
#define RRI8(r, s, t, imm, op0) \
    (((uint32_t)(imm) & 0xff) << 16 | (r) << 12 | (s) << 8 | (t) << 4 | (op0))

enum { M_L8UI = 0, M_L16UI = 1, M_L32I = 2, M_S8I = 4, M_S16I = 5, M_S32I = 6, M_L16SI = 9 };

static void i_rrr(int op2, int op1, int r, int s, int t) { o3(RRR(op2, op1, r, s, t)); }
static void i_add(int r, int s, int t) { i_rrr(8, 0, r, s, t); }
static void i_sub(int r, int s, int t) { i_rrr(0xc, 0, r, s, t); }
static void i_mov(int r, int s) { if (r != s) i_rrr(2, 0, r, s, s); } /* or r, s, s */
static void i_movi(int t, int imm) { o3(0x00a002 | t << 4 | ((imm >> 8) & 15) << 8 | (imm & 0xff) << 16); }
static void i_addi(int t, int s, int imm) { o3(RRI8(0xc, s, t, imm, 2)); }
static void i_addmi(int t, int s, int imm) { o3(RRI8(0xd, s, t, imm >> 8, 2)); }
static void i_mem(int op, int t, int s, int imm8) { o3(RRI8(op, s, t, imm8, 2)); }
static void i_ssl(int s) { i_rrr(4, 0, 1, s, 0); }
static void i_ssr(int s) { i_rrr(4, 0, 0, s, 0); }
static void i_sll(int r, int s) { i_rrr(0xa, 1, r, s, 0); }
static void i_srl(int r, int t) { i_rrr(9, 1, r, 0, t); }
static void i_sra(int r, int t) { i_rrr(0xb, 1, r, 0, t); }
static void i_extui(int r, int t, int sh, int bits) { i_rrr(bits - 1, 4 | (sh >> 4), r, sh & 15, t); }
static void i_sext(int r, int s, int bit) { i_rrr(2, 3, r, s, bit - 7); }
static void i_callx8(int s) { o3(0xe0 | s << 8); }
static void i_jx(int s) { o3(0xa0 | s << 8); }
static void i_retw(void) { o3(0x90); }
static void i_nop(void) { o3(0x20f0); }
static void i_entry(int s, int frame) { o3((uint32_t)(frame >> 3) << 12 | s << 8 | 0x36); }
static void i_j(int off) { o3(((uint32_t)off & 0x3ffff) << 6 | 6); } /* target = pc + 4 + off */
static void i_l32r(int t, int words) { o3(((uint32_t)words & 0xffff) << 8 | t << 4 | 1); }

static void i_slli(int r, int s, int sa) /* sa = 1..31 */
{
    int v = 32 - sa;
    i_rrr(v >> 4, 1, r, s, v & 15);
}

static void i_srli(int r, int t, int sa) /* sa = 1..31 */
{
    if (sa < 16)
        i_rrr(4, 1, r, sa, t);
    else
        i_extui(r, t, sa, 32 - sa);
}

static void i_srai(int r, int t, int sa) /* sa = 0..31 */
{
    i_rrr(2 | (sa >> 4), 1, r, sa & 15, t);
}

/* branches, taken to pc + 4 + imm */
static void i_bz(int m, int s, int imm) /* m: 0 beqz, 1 bnez, 2 bltz, 3 bgez */
{
    o3(((uint32_t)imm & 0xfff) << 12 | s << 8 | m << 6 | 1 << 4 | 6);
}

static void i_bi(int m, int s, int idx, int imm) /* m: 2 blti, 3 bgei; idx into b4const */
{
    o3(((uint32_t)imm & 0xff) << 16 | idx << 12 | s << 8 | m << 6 | 2 << 4 | 6);
}

static void i_br(int r, int s, int t, int imm) /* r: 1 beq 9 bne 2 blt 0xa bge 3 bltu 0xb bgeu */
{
    o3(RRI8(r, s, t, imm, 7));
}

/* ------------------------------------------------------------------------- */
/* constants, literals, addresses */

/* Load `v` into a: an inline literal word (relocated to sym + v when sym
   is set; the loader only applies a symbol's address, so the caller adds
   any offset in code), jumped over and read by L32R. */
static void emit_literal(int a, uint32_t v, Sym *sym)
{
    int start, lit, over;
    if (nocode_wanted)
        return;
    start = ind;
    lit = (start + 3 + 3) & ~3;
    over = lit + 4;
    i_j(over - (start + 4));
    if (over + 3 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, over + 3);
    memset(cur_text_section->data + ind, 0, lit - ind);
    write32le(cur_text_section->data + lit, sym ? 0 : v);
    if (sym)
        greloca(cur_text_section, sym, lit, R_XTENSA_32, v);
    ind = over;
    i_l32r(a, (lit - ((over + 3) & ~3)) >> 2);
}

static void load_const(int a, int v)
{
    if (v >= -2048 && v <= 2047)
        i_movi(a, v);
    else
        emit_literal(a, v, NULL);
}

/* d = s + imm */
static void emit_addi(int d, int s, int imm)
{
    if (imm >= -128 && imm <= 127) {
        if (imm)
            i_addi(d, s, imm);
        else
            i_mov(d, s);
    } else if (imm >= -32768 - 128 && imm <= 32512 + 127) {
        int hi = (imm + 128) & ~255, lo = imm - hi;
        i_addmi(d, s, hi);
        if (lo)
            i_addi(d, d, lo);
    } else if (d != s) {
        load_const(d, imm);
        i_add(d, s, d);
    } else if (d != A_T) {
        load_const(A_T, imm);
        i_add(d, d, A_T);
    } else {
        tcc_error("offset too large");
    }
}

/* Load or store `size` bytes between register t and [base + off]. */
static void mem_access(int op, int t, int base, int off, int size)
{
    if (off >= 0 && off <= 255 * size && !(off & (size - 1))) {
        i_mem(op, t, base, off / size);
    } else {
        emit_addi(A_T, base, off);
        i_mem(op, t, A_T, 0);
    }
}

static int access_size(int t)
{
    int bt = t & VT_BTYPE;
    if (bt == VT_BYTE || bt == VT_BOOL)
        return 1;
    if (bt == VT_SHORT)
        return 2;
    if (is_float(t))
        no_float();
    return 4;
}

/* Base register and offset of the address of lvalue sv; `tmp` may be used
   to compute it. */
static int lval_base(SValue *sv, int tmp, int *off)
{
    int v = sv->r & VT_VALMASK;
    int fc = sv->c.i;
    if (sv->r & VT_SYM) {
        emit_literal(tmp, 0, sv->sym);
        *off = fc;
        return tmp;
    }
    if (v == VT_LOCAL) {
        *off = FP_BIAS + fc;
        return A_FP;
    }
    if (v == VT_LLOCAL) {
        mem_access(M_L32I, tmp, A_FP, FP_BIAS + fc, 4);
        *off = 0;
        return tmp;
    }
    if (v == VT_CONST) {
        load_const(tmp, fc);
        *off = 0;
        return tmp;
    }
    if (v < VT_CONST) {
        /* the address is all in the register: TCC leaves stale values in
           c.i of register lvalues (as riscv64-gen.c notes) */
        *off = 0;
        return preg(v);
    }
    tcc_error("internal: bad lvalue");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* compares and jumps */

/* A 3-byte branch, taken to `dist` bytes after its start when `op` holds
   for a, b (b may be ZERO_REG). */
static void emit_bcond(int op, int a, int b, int dist)
{
    int imm = dist - 4;
    if (b == ZERO_REG) {
        switch (op) {
        case TOK_EQ: case TOK_ULE: i_bz(0, a, imm); return;
        case TOK_NE: case TOK_UGT: i_bz(1, a, imm); return;
        case TOK_LT: i_bz(2, a, imm); return;
        case TOK_GE: i_bz(3, a, imm); return;
        case TOK_LE: i_bi(2, a, 1, imm); return; /* blti a, 1 */
        case TOK_GT: i_bi(3, a, 1, imm); return; /* bgei a, 1 */
        case TOK_ULT: i_nop(); return;           /* never */
        case TOK_UGE: i_j(imm); return;          /* always */
        }
    } else {
        switch (op) {
        case TOK_EQ:  i_br(1, a, b, imm); return;
        case TOK_NE:  i_br(9, a, b, imm); return;
        case TOK_LT:  i_br(2, a, b, imm); return;
        case TOK_GE:  i_br(0xa, a, b, imm); return;
        case TOK_GT:  i_br(2, b, a, imm); return;
        case TOK_LE:  i_br(0xa, b, a, imm); return;
        case TOK_ULT: i_br(3, a, b, imm); return;
        case TOK_UGE: i_br(0xb, a, b, imm); return;
        case TOK_UGT: i_br(3, b, a, imm); return;
        case TOK_ULE: i_br(0xb, b, a, imm); return;
        }
    }
    tcc_error("internal: bad compare %d", op);
}

static void cmp_regs(SValue *sv, int *a, int *b)
{
    if (!sv->cmp_r)
        tcc_error("internal: compare without operands");
    *a = sv->cmp_r & 0xff;
    *b = sv->cmp_r >> 8;
}

static void set_cmp(int op, int a, int b)
{
    vset_VT_CMP(op);
    vtop->cmp_r = a | b << 8;
}

/* forward jumps are chained through their 24 bits until resolved */
ST_FUNC int gjmp(int t)
{
    int r;
    if (nocode_wanted)
        return t;
    if (t >= 1 << 24)
        tcc_error("code too large");
    r = ind;
    o3(t);
    return r;
}

ST_FUNC void gsym_addr(int t, int a)
{
    while (t) {
        int next = read24(t);
        int off = a - (t + 4);
        if (off < -(1 << 17) || off >= (1 << 17))
            tcc_error("jump out of range");
        write24(t, ((uint32_t)off & 0x3ffff) << 6 | 6);
        t = next;
    }
}

ST_FUNC void gjmp_addr(int a)
{
    int off = a - (ind + 4);
    if (off < -(1 << 17) || off >= (1 << 17))
        tcc_error("jump out of range");
    i_j(off);
}

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        int n1 = n, n2;
        while ((n2 = read24(n1)))
            n1 = n2;
        write24(n1, t);
        t = n;
    }
    return t;
}

/* jump to t if the compare on vtop holds */
ST_FUNC int gjmp_cond(int op, int t)
{
    int a, b;
    cmp_regs(vtop, &a, &b);
    emit_bcond(op ^ 1, a, b, 6); /* skip the jump if it doesn't */
    return gjmp(t);
}

/* ------------------------------------------------------------------------- */
/* load and store */

static void load_phys(int d, SValue *sv)
{
    int fr = sv->r;
    int v = fr & VT_VALMASK;
    int fc = sv->c.i;

    if (fr & VT_LVAL) {
        int size = access_size(sv->type.t), base, off;
        int uns = sv->type.t & VT_UNSIGNED;
        base = lval_base(sv, d, &off);
        if (size == 1) {
            mem_access(M_L8UI, d, base, off, 1);
            if (!uns && (sv->type.t & VT_BTYPE) != VT_BOOL)
                i_sext(d, d, 7);
        } else if (size == 2) {
            mem_access(uns ? M_L16UI : M_L16SI, d, base, off, 2);
        } else {
            mem_access(M_L32I, d, base, off, 4);
        }
    } else if (v == VT_CONST) {
        if (fr & VT_SYM) {
            emit_literal(d, 0, sv->sym);
            emit_addi(d, d, fc);
        } else {
            load_const(d, fc);
        }
    } else if (v == VT_LOCAL) {
        emit_addi(d, A_FP, FP_BIAS + fc);
    } else if (v < VT_CONST) {
        i_mov(d, preg(v));
    } else if (v == VT_CMP) {
        int a, b;
        cmp_regs(sv, &a, &b);
        emit_bcond(sv->cmp_op, a, b, 9); /* -> movi d, 1 */
        i_movi(d, 0);
        i_j(2);                          /* -> past movi d, 1 */
        i_movi(d, 1);
    } else if ((v & ~1) == VT_JMP) {
        int t = v & 1;
        i_movi(d, t);
        gjmp_addr(ind + 6);
        gsym(fc);
        i_movi(d, t ^ 1);
    } else {
        tcc_error("internal: load");
    }
}

ST_FUNC void load(int r, SValue *sv)
{
    if (is_float(sv->type.t))
        no_float();
    load_phys(preg(r), sv);
}

ST_FUNC void store(int r, SValue *sv)
{
    int size = access_size(sv->type.t), base, off, s = preg(r);
    base = lval_base(sv, A_T, &off);
    mem_access(size == 1 ? M_S8I : size == 2 ? M_S16I : M_S32I, s, base, off, size);
}

/* ------------------------------------------------------------------------- */
/* functions */

/* Copy the struct lvalue on vtop (`size` bytes, alignment `align`) to
   [base + off]. Leaves vtop in place. */
static void copy_struct(int base, int off, int size, int align)
{
    int p, i = 0;
    gaddrof();
    vtop->type = char_pointer_type;
    p = preg(gv(RC_INT));
    if (align >= 4)
        for (; i + 4 <= size; i += 4) {
            mem_access(M_L32I, A_C, p, i, 4);
            mem_access(M_S32I, A_C, base, off + i, 4);
        }
    for (; i < size; i++) {
        mem_access(M_L8UI, A_C, p, i, 1);
        mem_access(M_S8I, A_C, base, off + i, 1);
    }
}

static int arg_words(CType *type, int *align)
{
    int size = type_size(type, align);
    if (is_float(type->t))
        no_float();
    if (*align < 4)
        *align = 4;
    return (size + 3) >> 2;
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    int align, size = type_size(vt, &align);
    *ret_align = 4;
    *regsize = 4;
    if (size > 16)
        return 0; /* returned through a pointer passed as the first argument */
    ret->t = VT_INT;
    ret->ref = NULL;
    return (size + 3) >> 2;
}

/* Call: the function, then nb_args arguments, are on the value stack.
   Words of arguments go to a10..a15 (a2..a7 for the callee); an argument
   that doesn't fit, and all after it, go to the stack at sp + 0... */
ST_FUNC void gfunc_call(int nb_args)
{
    int i, j, n = nb_args, w = 0, k = 0, u, stack;
    int kreg[ARG_REGS];
    int *argw = tcc_malloc((n + 1) * sizeof(int));

    for (i = 0; i < n; i++) {
        int align, words = arg_words(&vtop[i - n + 1].type, &align);
        if (align > 4 && (w & 1))
            w++;
        if (w < ARG_REGS && w + words > ARG_REGS)
            w = ARG_REGS;
        argw[i] = w;
        w += words;
    }
    stack = w > ARG_REGS ? (w - ARG_REGS) * 4 : 0;
    if (stack > func_max_out)
        func_max_out = stack;

    save_regs(n + 1);

    /* first the stack arguments; register arguments stay on the value
       stack (structs and long longs as words) until all are known */
    u = n;
    for (i = 0; i < n; i++) {
        int align, size, bt, words, wpos = argw[i];
        vrotb(u + k);
        u--;
        bt = vtop->type.t & VT_BTYPE;
        size = type_size(&vtop->type, &align);
        words = arg_words(&vtop->type, &align);
        if (wpos >= ARG_REGS) {
            int off = (wpos - ARG_REGS) * 4;
            if (bt == VT_STRUCT) {
                copy_struct(A_SP, off, size, align);
            } else {
                gv(RC_INT);
                mem_access(M_S32I, preg(vtop->r), A_SP, off, 4);
                if (vtop->r2 < VT_CONST)
                    mem_access(M_S32I, preg(vtop->r2), A_SP, off + 4, 4);
            }
            vtop--;
        } else if (bt == VT_STRUCT) {
            int tmp;
            loc = (loc - words * 4) & -4;
            tmp = loc;
            copy_struct(A_FP, FP_BIAS + tmp, size, align);
            vtop--;
            for (j = 0; j < words; j++) {
                vset(&int_type, VT_LOCAL | VT_LVAL, tmp + 4 * j);
                kreg[k++] = wpos + j;
            }
        } else if (bt == VT_LLONG) {
            lexpand();
            kreg[k++] = wpos;
            kreg[k++] = wpos + 1;
        } else {
            kreg[k++] = wpos;
        }
    }
    tcc_free(argw);

    /* then each register argument into its register */
    for (i = 0; i < k; i++) {
        vrotb(k);
        gv(RC_R(kreg[i]));
    }

    /* and the function's address into a8 */
    vrotb(k + 1);
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && (vtop->r & VT_SYM)) {
        emit_literal(A_T, 0, vtop->sym);
        emit_addi(A_T, A_T, vtop->c.i);
    } else {
        load_phys(A_T, vtop);
    }
    vtop--;
    i_callx8(A_T);
    vtop -= k;
}

static void save_arg(int w, int at)
{
    if (w < ARG_REGS) {
        arg_save_reg[n_arg_save] = 2 + w;
        arg_save_loc[n_arg_save++] = at;
    }
}

ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym = func_type->ref;
    int w = 0, i;

    loc = 0;
    func_max_out = 0;
    n_arg_save = 0;
    func_vc = 0;
    func_prolog_ind = ind;
    for (i = 0; i < PROLOG_SIZE; i += 3)
        i_nop(); /* replaced in gfunc_epilog() */

    if (func_var) {
        /* all six argument registers in a row, for va_arg() */
        loc = -ARG_REGS * 4;
        va_reg_loc = loc;
        for (i = 0; i < ARG_REGS; i++)
            save_arg(i, va_reg_loc + 4 * i);
    }

    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        CType ret;
        int ra, rs;
        if (gfunc_sret(&func_vt, func_var, &ret, &ra, &rs) == 0) {
            if (func_var) {
                func_vc = va_reg_loc;
            } else {
                loc -= 4;
                func_vc = loc;
                save_arg(w, loc);
            }
            w++;
        }
    }
    while ((sym = sym->next) != NULL) {
        int align, words = arg_words(&sym->type, &align);
        if (align > 4 && (w & 1))
            w++;
        if (w < ARG_REGS && w + words > ARG_REGS)
            w = ARG_REGS;
        if (w >= ARG_REGS) {
            gfunc_set_param(sym, 32 + (w - ARG_REGS) * 4, 0);
        } else if (func_var) {
            gfunc_set_param(sym, va_reg_loc + 4 * w, 0);
        } else {
            loc = (loc - words * 4) & -align;
            for (i = 0; i < words; i++)
                save_arg(w + i, loc + 4 * i);
            gfunc_set_param(sym, loc, 0);
        }
        w += words;
    }
    /* GCC's va_start: past the named words; once on the stack, 32 bytes
       above __va_stk (which points 32 below the stack arguments) */
    va_ndx = w * 4 + (w >= ARG_REGS ? 8 : 0);
}

/* va_start(ap, last): vtop is the va_list lvalue ap. Fills in the three
   words and pops it. */
ST_FUNC void gen_va_start(void)
{
    int p;
    if (!func_var)
        tcc_error("va_start used in a function with fixed arguments");
    gaddrof();
    vtop->type = char_pointer_type;
    p = preg(gv(RC_INT));
    emit_addi(A_T, A_FP, FP_BIAS);              /* stack arguments - 32 */
    mem_access(M_S32I, A_T, p, 0, 4);
    emit_addi(A_T, A_FP, FP_BIAS + va_reg_loc); /* saved a2..a7 */
    mem_access(M_S32I, A_T, p, 4, 4);
    load_const(A_T, va_ndx);
    mem_access(M_S32I, A_T, p, 8, 4);
    vtop--;
}

ST_FUNC void gfunc_epilog(void)
{
    int bt = func_vt.t & VT_BTYPE, nret = 1, frame, saved, i;

    /* the return value: a10.. here, a2.. for the caller */
    if (bt == VT_VOID) {
        nret = 0;
    } else if (bt == VT_LLONG) {
        nret = 2;
    } else if (bt == VT_STRUCT) {
        CType ret;
        int ra, rs;
        nret = gfunc_sret(&func_vt, func_var, &ret, &ra, &rs);
    }
    for (i = 0; i < nret; i++)
        i_mov(2 + i, 10 + i);
    i_retw();

    frame = (-loc + func_max_out + 32 + 15) & -16;
    if (frame > 32760)
        tcc_error("stack frame too large (%d bytes)", frame);

    saved = ind;
    ind = func_prolog_ind;
    i_entry(A_SP, frame);
    emit_addi(A_T, A_SP, frame - 32 - FP_BIAS);
    for (i = 0; i < n_arg_save; i++)
        mem_access(M_S32I, arg_save_reg[i], A_T, FP_BIAS + arg_save_loc[i], 4);
    i_mov(A_FP, A_T);
    while (ind < func_prolog_ind + PROLOG_SIZE)
        i_nop();
    if (ind != func_prolog_ind + PROLOG_SIZE)
        tcc_error("internal: prologue");
    ind = saved;
}

ST_FUNC void gen_fill_nops(int bytes)
{
    while (bytes >= 3) {
        i_nop();
        bytes -= 3;
    }
    while (bytes-- > 0) {
        if (ind + 1 > cur_text_section->data_allocated)
            section_realloc(cur_text_section, ind + 1);
        cur_text_section->data[ind++] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* integer operations */

/* After the operands' registers are read: pop both, give the result
   (in vtop[-1]'s place) a fresh register. Returns its physical number. */
static int result_reg(void)
{
    int d;
    vtop -= 2;
    d = get_reg(RC_INT);
    vtop++;
    vtop->r = d;
    vtop->r2 = VT_CONST;
    return preg(d);
}

ST_FUNC void gen_opi(int op)
{
    int a, b, d;
    int is_cmp = op >= TOK_ULT && op <= TOK_GT;

    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
        int c = vtop->c.i;
        int fast = 0;
        switch (op) {
        case TOK_SHL: case TOK_SHR: case TOK_SAR:
            fast = 1;
            break;
        case '+':
            fast = c >= -128 && c <= 127;
            break;
        case '-':
            fast = c >= -127 && c <= 128;
            break;
        case '&':
            fast = c == 0xff || c == 0xffff;
            break;
        default:
            fast = is_cmp && c == 0 && vtop->c.i == 0;
            break;
        }
        if (fast) {
            vswap();
            a = preg(gv(RC_INT));
            vswap();
            if (is_cmp) {
                vtop -= 2;
                vtop++;
                set_cmp(op, a, ZERO_REG);
                return;
            }
            d = result_reg();
            switch (op) {
            case TOK_SHL:
                c &= 31;
                if (c) i_slli(d, a, c); else i_mov(d, a);
                break;
            case TOK_SHR:
                c &= 31;
                if (c) i_srli(d, a, c); else i_mov(d, a);
                break;
            case TOK_SAR:
                i_srai(d, a, c & 31);
                break;
            case '+':
                emit_addi(d, a, c);
                break;
            case '-':
                emit_addi(d, a, -c);
                break;
            case '&':
                i_extui(d, a, 0, c == 0xff ? 8 : 16);
                break;
            }
            return;
        }
    }

    gv2(RC_INT, RC_INT);
    a = preg(vtop[-1].r);
    b = preg(vtop[0].r);

    if (is_cmp) {
        vtop -= 2;
        vtop++;
        set_cmp(op, a, b);
        return;
    }

    if (op == TOK_UMULL) {
        int d2;
        d = result_reg();
        d2 = get_reg(RC_INT);
        vtop->r2 = d2;
        d2 = preg(d2);
        o3(RRR(0xa, 2, A_T, a, b)); /* muluh a8, a, b */
        o3(RRR(8, 2, d, a, b));     /* mull d, a, b */
        i_mov(d2, A_T);
        return;
    }

    d = result_reg();
    switch (op) {
    case '+':
        i_add(d, a, b);
        break;
    case '-':
        i_sub(d, a, b);
        break;
    case '&':
        i_rrr(1, 0, d, a, b);
        break;
    case '|':
        i_rrr(2, 0, d, a, b);
        break;
    case '^':
        i_rrr(3, 0, d, a, b);
        break;
    case '*':
        i_rrr(8, 2, d, a, b);    /* mull */
        break;
    case '/':
    case TOK_PDIV:
        i_rrr(0xd, 2, d, a, b);  /* quos */
        break;
    case '%':
        i_rrr(0xf, 2, d, a, b);  /* rems */
        break;
    case TOK_UDIV:
        i_rrr(0xc, 2, d, a, b);  /* quou */
        break;
    case TOK_UMOD:
        i_rrr(0xe, 2, d, a, b);  /* remu */
        break;
    case TOK_SHL:
        i_ssl(b);
        i_sll(d, a);
        break;
    case TOK_SHR:
        i_ssr(b);
        i_srl(d, a);
        break;
    case TOK_SAR:
        i_ssr(b);
        i_sra(d, a);
        break;
    case TOK_ADDC1: /* d = a + b, a9 = carry */
        i_add(A_T, a, b);
        i_movi(A_C, 0);
        i_br(0xb, A_T, a, 2);    /* bgeu a8, a, skip */
        i_movi(A_C, 1);
        i_mov(d, A_T);
        break;
    case TOK_ADDC2:
        i_add(d, a, b);
        i_add(d, d, A_C);
        break;
    case TOK_SUBC1: /* d = a - b, a9 = borrow */
        i_movi(A_C, 0);
        i_br(0xb, a, b, 2);      /* bgeu a, b, skip */
        i_movi(A_C, 1);
        i_sub(d, a, b);
        break;
    case TOK_SUBC2:
        i_sub(d, a, b);
        i_sub(d, d, A_C);
        break;
    default:
        tcc_error("internal: gen_opi %d", op);
    }
}

/* ------------------------------------------------------------------------- */
/* not supported */

ST_FUNC void gen_opf(int op) { no_float(); }
ST_FUNC void gen_cvt_itof(int t) { no_float(); }
ST_FUNC void gen_cvt_ftoi(int t) { no_float(); }
ST_FUNC void gen_cvt_ftof(int t) { no_float(); }

ST_FUNC void ggoto(void)
{
    gv(RC_INT);
    i_jx(preg(vtop->r));
    vtop--;
}

ST_FUNC void gen_vla_sp_save(int addr) { tcc_error("variable length arrays are not supported"); }
ST_FUNC void gen_vla_sp_restore(int addr) { tcc_error("variable length arrays are not supported"); }
ST_FUNC void gen_vla_alloc(CType *type, int align) { tcc_error("variable length arrays are not supported"); }

#endif /* !TARGET_DEFS_ONLY */
