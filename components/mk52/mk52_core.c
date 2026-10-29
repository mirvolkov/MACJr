/*
 * MK-52 calculator core. Every key (or prefixed key) maps to the same
 * two-digit command code the real calculator stores in program memory, and
 * exec_op() carries out a code whether it came from the keyboard (AUTO
 * mode) or from a running program. Codes that need an operand (a register
 * index, or a jump address in the next program byte) are completed by the
 * following key press.
 *
 * Numbers are doubles rounded to 8 significant digits after every
 * operation, with the MK range 1e-99 .. 9.9999999e99.
 */
#include "mk52_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_GAMMA "\x83" /* CP866 Г */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* What the next key press completes. */
enum {
  WAIT_NONE,
  WAIT_REG,   /* register index for STO/RCL and the K-indirect codes */
  WAIT_ADDR,  /* two-digit address after a jump code */
};

/* Codes that are followed by an address byte in program memory. */
static bool takes_address(uint8_t op) {
  switch (op) {
  case 0x51: case 0x53:                       /* БП, ПП */
  case 0x57: case 0x59: case 0x5C: case 0x5E: /* F x≠0, x≥0, x<0, x=0 */
  case 0x58: case 0x5A: case 0x5B: case 0x5D: /* F L2, L3, L1, L0 */
    return true;
  default:
    return false;
  }
}

/* ---------------------------------------------------------------- numbers */

/* Cut to 8 significant digits (the MK truncates, it does not round) and
 * apply the MK range. The small epsilon keeps binary noise such as
 * 0.29999999999999999 from losing its last digit. */
static double norm(mk52_t *m, double v) {
  if (isnan(v) || isinf(v)) {
    m->error = true;
    return 0.0;
  }
  if (v == 0.0) {
    return 0.0;
  }
  double a = fabs(v);
  int e = (int)floor(log10(a));
  double scale = pow(10.0, 7 - e);
  double r = floor(a * scale + 1e-5);
  if (r >= 1e8) { /* log10 landed one decade low */
    e++;
    scale = pow(10.0, 7 - e);
    r = floor(a * scale + 1e-5);
  }
  a = r / scale;
  if (a >= 1e100) {
    m->error = true;
    return 0.0;
  }
  if (a < 1e-99) {
    return 0.0;
  }
  return v < 0 ? -a : a;
}

static double entry_value(const mk52_t *m) {
  char buf[32];
  int n = 0;
  buf[n++] = m->neg ? '-' : '+';
  for (int i = 0; i < m->mant_len; i++) {
    if (m->has_point && i == m->point_pos) {
      buf[n++] = '.';
    }
    buf[n++] = m->mant[i];
  }
  if (m->mant_len == 0) {
    buf[n++] = '0';
  }
  n += snprintf(buf + n, sizeof(buf) - n, "e%c%u", m->exp_neg ? '-' : '+', m->exp);
  return strtod(buf, NULL);
}

static void push(mk52_t *m) {
  m->t = m->z;
  m->z = m->y;
  m->y = m->x;
}

static void drop(mk52_t *m) {
  m->y = m->z;
  m->z = m->t;
}

/* Turn the digits being typed into X. Every non-entry operation starts
 * with this. */
static void finish_entry(mk52_t *m) {
  if (m->entering) {
    m->x = norm(m, entry_value(m));
    m->entering = false;
  }
}

/* A digit, '.' or ВП that starts a new number. */
static void start_entry(mk52_t *m) {
  if (m->lift) {
    push(m);
  }
  m->error = false;
  m->entering = true;
  m->lift = true;
  m->exp_mode = m->neg = m->exp_neg = m->has_point = false;
  m->mant_len = 0;
  m->point_pos = 0;
  m->exp = 0;
}

static void enter_digit(mk52_t *m, int d) {
  if (!m->entering) {
    start_entry(m);
  }
  if (m->exp_mode) {
    m->exp = (uint8_t)((m->exp * 10 + d) % 100); /* new digit shifts in */
    return;
  }
  if (m->mant_len == 1 && m->mant[0] == '0' && !m->has_point) {
    m->mant_len = 0; /* no leading zeros */
  }
  if (m->mant_len < 8) {
    m->mant[m->mant_len++] = (char)('0' + d);
  }
}

/* ------------------------------------------------------------- functions */

static double to_rad(const mk52_t *m, double v) {
  switch (m->angle) {
  case MK_ANGLE_DEG:
    return v * M_PI / 180.0;
  case MK_ANGLE_GRD:
    return v * M_PI / 200.0;
  default:
    return v;
  }
}

static double from_rad(const mk52_t *m, double v) {
  switch (m->angle) {
  case MK_ANGLE_DEG:
    return v * 180.0 / M_PI;
  case MK_ANGLE_GRD:
    return v * 200.0 / M_PI;
  default:
    return v;
  }
}

/* Sexagesimal conversions. H.MM -> H + MM/60 etc.; minutes / seconds must
 * be below 60. */
static double dms_to_deg(mk52_t *m, double v, bool with_seconds) {
  double a = fabs(v);
  double h = floor(a);
  double frac = (a - h) * 100.0;
  double mm = floor(frac + 1e-9);
  double r;
  if (with_seconds) {
    double ss = (frac - mm) * 100.0;
    if (mm >= 60 || ss >= 60) {
      m->error = true;
    }
    r = h + mm / 60.0 + ss / 3600.0;
  } else {
    if (frac >= 60) {
      m->error = true;
    }
    r = h + frac / 60.0;
  }
  return v < 0 ? -r : r;
}

static double deg_to_dms(double v, bool with_seconds) {
  double a = fabs(v);
  double h = floor(a);
  double minutes = (a - h) * 60.0;
  double r;
  if (with_seconds) {
    double mm = floor(minutes + 1e-9);
    double ss = (minutes - mm) * 60.0;
    r = h + mm / 100.0 + ss / 10000.0;
  } else {
    r = h + minutes / 100.0;
  }
  return v < 0 ? -r : r;
}

/* Unary function on X: X1 = X, X = f(X). */
static void unary(mk52_t *m, double r) {
  m->x1 = m->x;
  m->x = norm(m, r);
}

/* Binary arithmetic: X = Y op X, stack drops. */
static void binary(mk52_t *m, double r) {
  m->x1 = m->x;
  drop(m);
  m->x = norm(m, r);
}

/* Modify an indirect pointer register (RG0-3: -1, RG4-6: +1, others kept)
 * and return its integer value. */
static int indirect(mk52_t *m, int n) {
  if (n <= 3) {
    m->reg[n] = norm(m, m->reg[n] - 1);
  } else if (n <= 6) {
    m->reg[n] = norm(m, m->reg[n] + 1);
  }
  return (int)fabs(trunc(m->reg[n]));
}

static double random01(mk52_t *m) {
  m->rng = m->rng * 1103515245u + 12345u;
  return (double)((m->rng >> 8) % 100000000u) / 1e8;
}

/* Execute a data / function code (everything except program flow). */
static void exec_op(mk52_t *m, uint8_t op) {
  double x;

  if (op <= 0x09) {
    enter_digit(m, op);
    return;
  }
  switch (op) {
  case 0x0A: /* . */
    if (!m->entering) {
      start_entry(m);
      m->mant[0] = '0';
      m->mant_len = 1;
    }
    if (!m->exp_mode && !m->has_point) {
      if (m->mant_len == 0) {
        m->mant[m->mant_len++] = '0';
      }
      m->has_point = true;
      m->point_pos = m->mant_len;
    }
    return;
  case 0x0B: /* /-/ */
    if (m->entering) {
      if (m->exp_mode) {
        m->exp_neg = !m->exp_neg;
      } else {
        m->neg = !m->neg;
      }
    } else {
      m->x = -m->x;
    }
    return;
  case 0x0C: /* ВП */
    if (!m->entering) {
      start_entry(m);
    }
    if (m->mant_len == 0 || entry_value(m) == 0.0) {
      m->mant[0] = '1';
      m->mant_len = 1;
      m->has_point = false;
    }
    m->exp_mode = true;
    return;
  case 0x0D: /* Cx */
    m->entering = false;
    m->error = false;
    m->x = 0.0;
    m->lift = false;
    return;
  case 0x0E: /* В↑ */
    finish_entry(m);
    push(m);
    m->lift = false;
    return;
  default:
    break;
  }

  finish_entry(m);
  m->lift = true;
  x = m->x;

  if (op >= 0x40 && op <= 0x4E) { /* x→П n */
    m->reg[op - 0x40] = x;
    return;
  }
  if (op >= 0x60 && op <= 0x6E) { /* П→x n */
    push(m);
    m->x = m->reg[op - 0x60];
    return;
  }
  if (op >= 0xB0 && op <= 0xBE) { /* K x→П (n) */
    int r = indirect(m, op - 0xB0);
    if (r < MK52_REGS) {
      m->reg[r] = x;
    }
    return;
  }
  if (op >= 0xD0 && op <= 0xDE) { /* K П→x (n) */
    int r = indirect(m, op - 0xD0);
    push(m);
    m->x = r < MK52_REGS ? m->reg[r] : 0.0;
    return;
  }

  switch (op) {
  case 0x0F: /* F Bx */
    push(m);
    m->x = m->x1;
    break;
  case 0x10: binary(m, m->y + x); break;
  case 0x11: binary(m, m->y - x); break;
  case 0x12: binary(m, m->y * x); break;
  case 0x13:
    if (x == 0.0) {
      m->error = true;
    } else {
      binary(m, m->y / x);
    }
    break;
  case 0x14: /* ↔ */
    m->x1 = x;
    m->x = m->y;
    m->y = x;
    break;
  case 0x15: unary(m, pow(10.0, x)); break;
  case 0x16: unary(m, exp(x)); break;
  case 0x17:
    if (x <= 0) m->error = true; else unary(m, log10(x));
    break;
  case 0x18:
    if (x <= 0) m->error = true; else unary(m, log(x));
    break;
  case 0x19:
    if (fabs(x) > 1) m->error = true; else unary(m, from_rad(m, asin(x)));
    break;
  case 0x1A:
    if (fabs(x) > 1) m->error = true; else unary(m, from_rad(m, acos(x)));
    break;
  case 0x1B: unary(m, from_rad(m, atan(x))); break;
  case 0x1C: unary(m, sin(to_rad(m, x))); break;
  case 0x1D: unary(m, cos(to_rad(m, x))); break;
  case 0x1E: {
    double c = cos(to_rad(m, x));
    if (fabs(c) < 1e-12) m->error = true; else unary(m, sin(to_rad(m, x)) / c);
    break;
  }
  case 0x20: /* F π: the MK's constant is truncated, not rounded */
    push(m);
    m->x = 3.1415926;
    break;
  case 0x21:
    if (x < 0) m->error = true; else unary(m, sqrt(x));
    break;
  case 0x22: unary(m, x * x); break;
  case 0x23:
    if (x == 0) m->error = true; else unary(m, 1.0 / x);
    break;
  case 0x24: /* F x^y: base in X, exponent in Y, Y kept */
    if (x <= 0) {
      m->error = true;
    } else {
      m->x1 = x;
      m->x = norm(m, pow(x, m->y));
    }
    break;
  case 0x25: /* F ⟳: rotate the stack down */
    m->x1 = x;
    m->x = m->y;
    m->y = m->z;
    m->z = m->t;
    m->t = x;
    break;
  case 0x26: unary(m, dms_to_deg(m, x, false)); break; /* K °′→° */
  case 0x2A: unary(m, deg_to_dms(x, true)); break;     /* K °→°′″ */
  case 0x30: unary(m, dms_to_deg(m, x, true)); break;  /* K °′″→° */
  case 0x31: unary(m, fabs(x)); break;                 /* K |x| */
  case 0x32: unary(m, x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0)); break; /* K ЗН */
  case 0x33: unary(m, deg_to_dms(x, false)); break;    /* K °→°′ */
  case 0x34: unary(m, trunc(x)); break;                /* K [x] */
  case 0x35: unary(m, x - trunc(x)); break;            /* K {x} */
  case 0x36: binary(m, m->y > x ? m->y : x); break;    /* K max */
  case 0x37: case 0x38: case 0x39: case 0x3A:          /* K ∧ ∨ ⊕ ИНВ */
    m->error = true;                                   /* not emulated */
    break;
  case 0x3B: /* K СЧ */
    push(m);
    m->x = norm(m, random01(m));
    break;
  default: /* K НОП and undocumented codes */
    break;
  }
}

/* ------------------------------------------------------------ programs */

static uint8_t addr_of(uint8_t code) {
  int a = (code >> 4) * 10 + (code & 0x0F); /* A0..A4 -> 100..104 */
  return (uint8_t)(a < MK52_PROG_SIZE ? a : 0);
}

static uint8_t next_pc(uint8_t pc, int by) {
  return (uint8_t)((pc + by) % MK52_PROG_SIZE);
}

static void call(mk52_t *m, uint8_t ret, uint8_t to) {
  if (m->ret_n == MK52_RET_DEPTH) { /* oldest return address falls out */
    memmove(m->ret, m->ret + 1, MK52_RET_DEPTH - 1);
    m->ret_n--;
  }
  m->ret[m->ret_n++] = ret;
  m->pc = to;
}

static bool cond_true(const mk52_t *m, uint8_t kind) {
  switch (kind) {
  case 0x57: case 0x70: return m->x != 0;
  case 0x59: case 0x90: return m->x >= 0;
  case 0x5C: case 0xC0: return m->x < 0;
  default:              return m->x == 0; /* 0x5E, 0xE0 */
  }
}

bool mk52_step(mk52_t *m) {
  uint8_t op = m->prog[m->pc];
  uint8_t arg = m->prog[next_pc(m->pc, 1)];
  uint8_t hi = op & 0xF0;
  int n = op & 0x0F;

  if (op == 0x50) { /* С/П */
    m->pc = next_pc(m->pc, 1);
    m->running = false;
    return false;
  }
  if (op == 0x51) { /* БП */
    m->pc = addr_of(arg);
  } else if (op == 0x52) { /* В/О */
    m->pc = m->ret_n ? m->ret[--m->ret_n] : 1;
  } else if (op == 0x53) { /* ПП */
    call(m, next_pc(m->pc, 2), addr_of(arg));
  } else if (op == 0x57 || op == 0x59 || op == 0x5C || op == 0x5E) {
    finish_entry(m);
    m->pc = cond_true(m, op) ? next_pc(m->pc, 2) : addr_of(arg);
  } else if (op == 0x58 || op == 0x5A || op == 0x5B || op == 0x5D) {
    int r = op == 0x5D ? 0 : op == 0x5B ? 1 : op == 0x58 ? 2 : 3;
    m->reg[r] = norm(m, m->reg[r] - 1);
    m->pc = m->reg[r] >= 1 ? addr_of(arg) : next_pc(m->pc, 2);
  } else if (n <= 0x0E && (hi == 0x70 || hi == 0x90 || hi == 0xC0 || hi == 0xE0)) {
    finish_entry(m); /* K conditions: jump through the register if false */
    if (cond_true(m, hi)) {
      m->pc = next_pc(m->pc, 1);
    } else {
      m->pc = (uint8_t)(indirect(m, n) % MK52_PROG_SIZE);
    }
  } else if (n <= 0x0E && hi == 0x80) { /* K БП (n) */
    m->pc = (uint8_t)(indirect(m, n) % MK52_PROG_SIZE);
  } else if (n <= 0x0E && hi == 0xA0) { /* K ПП (n) */
    call(m, next_pc(m->pc, 1), (uint8_t)(indirect(m, n) % MK52_PROG_SIZE));
  } else {
    m->pc = next_pc(m->pc, 1);
    exec_op(m, op);
  }

  if (m->error) {
    m->running = false;
  }
  return m->running;
}

/* ------------------------------------------------------------ keyboard */

static const uint8_t s_plain[MK_KEY_COUNT] = {
  [MK_KEY_0] = 0x00, [MK_KEY_1] = 0x01, [MK_KEY_2] = 0x02, [MK_KEY_3] = 0x03,
  [MK_KEY_4] = 0x04, [MK_KEY_5] = 0x05, [MK_KEY_6] = 0x06, [MK_KEY_7] = 0x07,
  [MK_KEY_8] = 0x08, [MK_KEY_9] = 0x09, [MK_KEY_DOT] = 0x0A, [MK_KEY_NEG] = 0x0B,
  [MK_KEY_EE] = 0x0C, [MK_KEY_CX] = 0x0D, [MK_KEY_ENTER] = 0x0E,
  [MK_KEY_PLUS] = 0x10, [MK_KEY_MINUS] = 0x11, [MK_KEY_MUL] = 0x12,
  [MK_KEY_DIV] = 0x13, [MK_KEY_SWAP] = 0x14,
  [MK_KEY_SP] = 0x50, [MK_KEY_BP] = 0x51, [MK_KEY_VO] = 0x52, [MK_KEY_PP] = 0x53,
};

/* F + key. 0xFF = no function. АВТ / ПРГ / CF are handled separately. */
static const uint8_t s_f[MK_KEY_COUNT] = {
  [MK_KEY_0] = 0x15, [MK_KEY_1] = 0x16, [MK_KEY_2] = 0x17, [MK_KEY_3] = 0x18,
  [MK_KEY_4] = 0x19, [MK_KEY_5] = 0x1A, [MK_KEY_6] = 0x1B, [MK_KEY_7] = 0x1C,
  [MK_KEY_8] = 0x1D, [MK_KEY_9] = 0x1E,
  [MK_KEY_PLUS] = 0x20, [MK_KEY_MINUS] = 0x21, [MK_KEY_MUL] = 0x22,
  [MK_KEY_DIV] = 0x23, [MK_KEY_SWAP] = 0x24, [MK_KEY_DOT] = 0x25,
  [MK_KEY_ENTER] = 0x0F,
  [MK_KEY_SST] = 0x5C, [MK_KEY_BST] = 0x5E, [MK_KEY_VO] = 0x59, [MK_KEY_SP] = 0x57,
  [MK_KEY_RCL] = 0x5D, [MK_KEY_STO] = 0x5B, [MK_KEY_BP] = 0x58, [MK_KEY_PP] = 0x5A,
  [MK_KEY_NEG] = 0xFF, [MK_KEY_EE] = 0xFF, [MK_KEY_CX] = 0xFF,
  [MK_KEY_F] = 0xFF, [MK_KEY_K] = 0xFF, [MK_KEY_A_UP] = 0xFF, [MK_KEY_UPDOWN] = 0xFF,
};

/* K + key. Values 0x70..0xE0 with low nibble 0 are the indirect families,
 * completed by a register key. */
static const uint8_t s_k[MK_KEY_COUNT] = {
  [MK_KEY_0] = 0x54, [MK_KEY_1] = 0x55, [MK_KEY_2] = 0x56, [MK_KEY_3] = 0x30,
  [MK_KEY_4] = 0x31, [MK_KEY_5] = 0x32, [MK_KEY_6] = 0x33, [MK_KEY_7] = 0x34,
  [MK_KEY_8] = 0x35, [MK_KEY_9] = 0x36, [MK_KEY_DOT] = 0x37, [MK_KEY_NEG] = 0x38,
  [MK_KEY_EE] = 0x39, [MK_KEY_CX] = 0x3A, [MK_KEY_ENTER] = 0x3B,
  [MK_KEY_PLUS] = 0x26, [MK_KEY_MINUS] = 0x27, [MK_KEY_MUL] = 0x28,
  [MK_KEY_DIV] = 0x29, [MK_KEY_SWAP] = 0x2A,
  [MK_KEY_SP] = 0x70, [MK_KEY_BP] = 0x80, [MK_KEY_VO] = 0x90, [MK_KEY_PP] = 0xA0,
  [MK_KEY_STO] = 0xB0, [MK_KEY_SST] = 0xC0, [MK_KEY_RCL] = 0xD0, [MK_KEY_BST] = 0xE0,
  [MK_KEY_F] = 0xFF, [MK_KEY_K] = 0xFF, [MK_KEY_A_UP] = 0xFF, [MK_KEY_UPDOWN] = 0xFF,
};

/* Register index typed after x→П / П→x: 0-9, then . /-/ ВП Cx В↑ = a-e. */
static int reg_of_key(mk52_key_t key) {
  if (key <= MK_KEY_9) {
    return key - MK_KEY_0;
  }
  if (key >= MK_KEY_DOT && key <= MK_KEY_ENTER) {
    return 10 + (key - MK_KEY_DOT);
  }
  return -1;
}

static void prog_store(mk52_t *m, uint8_t code) {
  m->prog[m->pc] = code;
  m->pc = next_pc(m->pc, 1);
}

/* An opcode produced by the keyboard: store it (PRG) or act on it (AUTO). */
static void key_op(mk52_t *m, uint8_t op) {
  if (m->mode == MK_MODE_PRG) {
    prog_store(m, op);
    if (takes_address(op)) {
      m->wait = WAIT_ADDR;
      m->addr_hi = -1;
    }
    return;
  }

  /* AUTO mode */
  switch (op) {
  case 0x50: /* С/П: run from pc */
    finish_entry(m);
    m->running = true;
    return;
  case 0x51: /* БП: the next two digits set pc */
    m->wait = WAIT_ADDR;
    m->wait_op = 0x51;
    m->addr_hi = -1;
    return;
  case 0x52: /* В/О */
    m->pc = 0;
    m->ret_n = 0;
    return;
  case 0x53: /* ПП: single step */
    m->running = true;
    mk52_step(m);
    m->running = false;
    return;
  default:
    break;
  }
  if (takes_address(op)) {
    return; /* conditions / loops do nothing outside a program */
  }
  if (op >= 0x70 && (op & 0xF0) != 0xB0 && (op & 0xF0) != 0xD0) {
    /* K БП / K ПП (n) jump; K conditions are ignored */
    if ((op & 0xF0) == 0x80 || (op & 0xF0) == 0xA0) {
      m->pc = (uint8_t)(indirect(m, op & 0x0F) % MK52_PROG_SIZE);
    }
    return;
  }
  exec_op(m, op);
}

void mk52_key(mk52_t *m, mk52_key_t key) {
  if (key >= MK_KEY_COUNT) {
    return;
  }
  if (m->running) { /* while a program runs only С/П does anything: stop */
    if (key == MK_KEY_SP) {
      m->running = false;
    }
    return;
  }

  /* Second half of a two-key sequence. */
  if (m->wait == WAIT_REG) {
    int r = reg_of_key(key);
    m->wait = WAIT_NONE;
    if (r >= 0) {
      key_op(m, (uint8_t)(m->wait_op + r));
    }
    return;
  }
  if (m->wait == WAIT_ADDR) {
    int d = key <= MK_KEY_9 ? (int)key : (key == MK_KEY_DOT ? 10 : -1);
    if (d < 0) {
      m->wait = WAIT_NONE;
      return;
    }
    if (m->addr_hi < 0) {
      m->addr_hi = (int8_t)d;
      return;
    }
    uint8_t code = (uint8_t)((m->addr_hi << 4) | (d > 9 ? 0 : d));
    m->wait = WAIT_NONE;
    if (m->mode == MK_MODE_PRG) {
      prog_store(m, code);
    } else {
      m->pc = addr_of(code);
    }
    return;
  }

  uint8_t prefix = m->prefix;
  m->prefix = 0;

  if (key == MK_KEY_F) {
    m->prefix = 'F';
    return;
  }
  if (key == MK_KEY_K) {
    m->prefix = 'K';
    return;
  }
  if (key == MK_KEY_A_UP && prefix == 0) {
    m->angle = (mk52_angle_t)((m->angle + 1) % 3);
    return;
  }

  if (prefix == 'F') {
    switch (key) {
    case MK_KEY_NEG: /* F АВТ */
      m->mode = MK_MODE_AUTO;
      return;
    case MK_KEY_EE: /* F ПРГ */
      finish_entry(m);
      m->mode = MK_MODE_PRG;
      return;
    case MK_KEY_CX: /* F CF */
      return;
    default:
      if (s_f[key] != 0xFF) {
        key_op(m, s_f[key]);
      }
      return;
    }
  }

  if (prefix == 'K') {
    uint8_t op = s_k[key];
    if (op == 0xFF) {
      return;
    }
    if (op >= 0x70 && (op & 0x0F) == 0) { /* indirect family: needs a register */
      m->wait = WAIT_REG;
      m->wait_op = op;
      return;
    }
    key_op(m, op);
    return;
  }

  switch (key) {
  case MK_KEY_STO:
    m->wait = WAIT_REG;
    m->wait_op = 0x40;
    return;
  case MK_KEY_RCL:
    m->wait = WAIT_REG;
    m->wait_op = 0x60;
    return;
  case MK_KEY_SST:
    m->pc = next_pc(m->pc, 1);
    return;
  case MK_KEY_BST:
    m->pc = (uint8_t)(m->pc ? m->pc - 1 : MK52_PROG_SIZE - 1);
    return;
  case MK_KEY_A_UP:
  case MK_KEY_UPDOWN:
    return;
  default:
    key_op(m, s_plain[key]);
    return;
  }
}

/* ------------------------------------------------------------- display */

void mk52_code_str(uint8_t code, char out[3]) {
  static const char *glyph = "0123456789-LC" CH_GAMMA "E ";
  out[0] = glyph[code >> 4];
  out[1] = glyph[code & 0x0F];
  out[2] = '\0';
}

static void format_entry(const mk52_t *m, char *out, size_t len) {
  char mant[12];
  int n = 0;
  for (int i = 0; i < m->mant_len; i++) {
    if (m->has_point && i == m->point_pos) {
      mant[n++] = '.';
    }
    mant[n++] = m->mant[i];
  }
  if (m->mant_len == 0) {
    mant[n++] = '0';
  }
  if (!m->has_point || m->point_pos == m->mant_len) {
    mant[n++] = '.';
  }
  mant[n] = '\0';
  if (m->exp_mode) {
    snprintf(out, len, "%s%s %s%02u", m->neg ? "-" : "", mant, m->exp_neg ? "-" : " ", m->exp);
  } else {
    snprintf(out, len, "%s%s", m->neg ? "-" : "", mant);
  }
}

static void format_number(double v, char *out, size_t len) {
  if (v == 0.0) {
    snprintf(out, len, "0.");
    return;
  }
  double a = fabs(v);
  const char *sign = v < 0 ? "-" : "";
  char buf[24];
  if (a >= 1.0 && a < 1e8) { /* natural form, 8 significant digits */
    int int_digits = (int)floor(log10(a)) + 1;
    if (int_digits > 8) {
      int_digits = 8;
    }
    snprintf(buf, sizeof(buf), "%.*f", 8 - int_digits, a);
    if (strchr(buf, '.')) {
      size_t l = strlen(buf);
      while (buf[l - 1] == '0') {
        buf[--l] = '\0';
      }
    } else {
      strcat(buf, ".");
    }
    snprintf(out, len, "%s%s", sign, buf);
    return;
  }
  /* floating form: m.mmmmmmm and a two-digit exponent */
  snprintf(buf, sizeof(buf), "%.7e", a);
  char *e = strchr(buf, 'e');
  int exponent = atoi(e + 1);
  *e = '\0';
  size_t l = strlen(buf);
  while (buf[l - 1] == '0') {
    buf[--l] = '\0';
  }
  snprintf(out, len, "%s%s%c%02d", sign, buf, exponent < 0 ? '-' : ' ', abs(exponent));
}

void mk52_display(const mk52_t *m, char *out, size_t len) {
  if (m->mode == MK_MODE_PRG) {
    char c1[3], c2[3], c3[3], a[3];
    mk52_code_str(m->prog[(m->pc + MK52_PROG_SIZE - 1) % MK52_PROG_SIZE], c1);
    mk52_code_str(m->prog[(m->pc + MK52_PROG_SIZE - 2) % MK52_PROG_SIZE], c2);
    mk52_code_str(m->prog[(m->pc + MK52_PROG_SIZE - 3) % MK52_PROG_SIZE], c3);
    mk52_code_str((uint8_t)(((m->pc / 10) << 4) | (m->pc % 10)), a);
    snprintf(out, len, "%s %s %s  %s", c1, c2, c3, a);
    return;
  }
  if (m->error) {
    snprintf(out, len, "E" CH_GAMMA CH_GAMMA "O" CH_GAMMA);
    return;
  }
  if (m->entering) {
    format_entry(m, out, len);
  } else {
    format_number(m->x, out, len);
  }
}

void mk52_reset(mk52_t *m) {
  memset(m, 0, sizeof(*m));
  m->angle = MK_ANGLE_DEG;
  m->addr_hi = -1;
  m->rng = 0x52;
}
