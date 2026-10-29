/*
 * Elektronika MK-52 calculator core: stack, registers, number entry,
 * functions and the 105-step program memory with the MK-61/MK-52 command
 * codes. Pure C with no hardware or RTOS dependencies -- the UI feeds keys
 * in and reads the display string out.
 *
 * Behaviour follows the MK-52 / MK-61 operating manuals. Not emulated:
 * the EEPROM (A-up / up-down keys), bitwise logic (K AND/OR/XOR/NOT show
 * EGGOG), undocumented codes, and exact last-digit results of the original
 * chips (we use double precision rounded to 8 significant digits).
 */
#ifndef MACJR_MK52_CORE_H
#define MACJR_MK52_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MK52_PROG_SIZE 105
#define MK52_REGS 15
#define MK52_RET_DEPTH 5

/* The 32 physical keys. */
typedef enum {
  MK_KEY_0, MK_KEY_1, MK_KEY_2, MK_KEY_3, MK_KEY_4,
  MK_KEY_5, MK_KEY_6, MK_KEY_7, MK_KEY_8, MK_KEY_9,
  MK_KEY_DOT,    /* .    register a */
  MK_KEY_NEG,    /* /-/  register b */
  MK_KEY_EE,     /* ВП   register c */
  MK_KEY_CX,     /* Cx   register d */
  MK_KEY_ENTER,  /* В↑   register e */
  MK_KEY_SWAP,   /* ↔ */
  MK_KEY_PLUS, MK_KEY_MINUS, MK_KEY_MUL, MK_KEY_DIV,
  MK_KEY_F, MK_KEY_K,
  MK_KEY_STO,    /* x→П */
  MK_KEY_RCL,    /* П→x */
  MK_KEY_SST,    /* ШГ→ */
  MK_KEY_BST,    /* ←ШГ */
  MK_KEY_BP,     /* БП  goto */
  MK_KEY_PP,     /* ПП  gosub / single step */
  MK_KEY_VO,     /* В/О return / pc = 0 */
  MK_KEY_SP,     /* С/П run / stop */
  MK_KEY_A_UP,   /* А↑ (EEPROM address; here: cycle the angle mode) */
  MK_KEY_UPDOWN, /* ↑↓ (EEPROM exchange; not emulated) */
  MK_KEY_COUNT
} mk52_key_t;

typedef enum { MK_ANGLE_RAD, MK_ANGLE_GRD, MK_ANGLE_DEG } mk52_angle_t;
typedef enum { MK_MODE_AUTO, MK_MODE_PRG } mk52_mode_t;

typedef struct {
  double x, y, z, t, x1;    /* stack and previous-X register */
  double reg[MK52_REGS];    /* RG0..RG9, RGa..RGe */
  uint8_t prog[MK52_PROG_SIZE];
  uint8_t pc;               /* address counter, 0..104 */
  uint8_t ret[MK52_RET_DEPTH];
  uint8_t ret_n;
  mk52_mode_t mode;
  mk52_angle_t angle;
  bool running;
  bool error;               /* display shows ЕГГОГ */

  /* keyboard state */
  uint8_t prefix;           /* 0, 'F' or 'K' */
  uint8_t wait;             /* what the next key completes, see mk52_core.c */
  uint8_t wait_op;          /* opcode waiting for its register / address */
  int8_t addr_hi;           /* first digit of a two-digit address, -1 = none */

  /* number entry */
  bool entering;            /* digits are being typed into X */
  bool lift;                /* the next new number pushes the stack first */
  bool exp_mode;            /* typing the exponent (after ВП) */
  bool neg;
  bool exp_neg;
  bool has_point;
  char mant[9];
  uint8_t mant_len;
  uint8_t point_pos;
  uint8_t exp;

  uint32_t rng;
} mk52_t;

/* Power-on state: everything zero, AUTO mode, degrees. */
void mk52_reset(mk52_t *m);

/* Feed one key press. In AUTO mode with С/П this sets m->running; the
 * caller then calls mk52_step() until it returns false. */
void mk52_key(mk52_t *m, mk52_key_t key);

/* Execute one program step. Returns false once the program has stopped
 * (С/П, error). */
bool mk52_step(mk52_t *m);

/* Text of the 12-position VFD: "-1.2345678-01", "12.56637", "ЕГГОГ" (with
 * CP866 Г) or, in PRG mode, "KK KK KK  AA". */
void mk52_display(const mk52_t *m, char *out, size_t len);

/* Two-character form of a program byte / address, using the MK glyphs for
 * hex digits A-F: - L C Г E (blank). */
void mk52_code_str(uint8_t code, char out[3]);

#endif
