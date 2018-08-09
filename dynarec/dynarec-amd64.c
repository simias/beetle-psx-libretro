#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "dynarec-compiler.h"

/* AMD64 register encoding.
 *
 * PAFC = Preserved Across Function Calls, per the x86-64 ABI:
 *
 * http://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
 */
enum X86_REG {
   REG_AX  = 0,  /* Temporary variable, return value 0 */
   REG_BX  = 3,  /* Dynarec temporary register (DT) [PAFC] */
   REG_CX  = 1,  /* Cycle counter, func arg 3 */
   REG_DX  = 2,  /* Temporary variable, func arg 2, return value 1 */
   REG_BP  = 5,  /* Host BP [PAFC] */
   REG_SI  = 6,  /* Temporary variable, func arg 1 */
   REG_DI  = 7,  /* struct dynarec_state pointer, func arg 0 */
   REG_SP  = 4,  /* Host stack [PAFC] */
   REG_R8  = 8,  /* PSX AT */
   REG_R9  = 9,  /* PSX V0 */
   REG_R10 = 10, /* PSX V1 */
   REG_R11 = 11, /* PSX A0 */
   REG_R12 = 12, /* PSX A1 [PAFC] */
   REG_R13 = 13, /* PSX T0 [PAFC] */
   REG_R14 = 14, /* PSX SP [PAFC] */
   REG_R15 = 15, /* PSX RA [PAFC] */
};

#define STATE_REG  REG_DI

/* Returns the host register location for the PSX-emulated register
   `reg`. Returns -1 if no host register is allocated, in which case
   it must be accessed in memory.

   If you change this don't forget to change `dynarec_execute` as
   well. */
static int register_location(enum PSX_REG reg) {
   switch (reg) {
   case PSX_REG_AT:
      return REG_R8;
   case PSX_REG_V0:
      return REG_R9;
   case PSX_REG_V1:
      return REG_R10;
   case PSX_REG_A0:
      return REG_R11;
   case PSX_REG_A1:
      return REG_R12;
   case PSX_REG_T0:
      return REG_R13;
   case PSX_REG_SP:
      return REG_R14;
   case PSX_REG_RA:
      return REG_R15;
   case PSX_REG_DT:
      return REG_BX;
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                      \
      printf("%s:%d not implemented\n", __func__, __LINE__);    \
      abort();                                                  \
   } while (0)

/*******************************************
 * Helper "assembler" functions and macros *
 *******************************************/

/* A set of rather ugly macros to generate if/else statements. The
 * "else" part can be ommited. These statemens introduce a new scope
 * and can be nested. These macros use the 2 byte jump instructions so
 * the bodies must not be bigger than 127 bytes.
 *
 * "else if" statements can be implementing by nesting the elses:
 *
 *   if (a) {                       if (a) {
 *      x();                           x();
 *   } else if (b) {   =======>     } else {
 *      y();                           if (b) {
 *   } else {                             y();
 *      z();                           } else {
 *   }                                    z();
 *                                     }
 *                                  }
 */
#define IF(_opcode) do {                        \
   uint8_t *_jump_patch;                        \
   *((compiler)->map++) = (_opcode);            \
   _jump_patch = (compiler)->map++;

#define ELSE {                                                  \
      uint32_t _jump_off = ((compiler)->map - _jump_patch) + 1; \
      assert(_jump_off < 128);                                  \
      *_jump_patch = _jump_off;                                 \
      /* JMP imms8 */                                           \
      *((compiler)->map++) = 0xeb;                              \
      _jump_patch = (compiler)->map++;                          \
   }

#define ENDIF {                                                 \
      uint32_t _jump_off = ((compiler)->map - _jump_patch) - 1; \
      assert(_jump_off < 128);                                  \
      *_jump_patch = _jump_off;                                 \
   }} while (0)

/* The meaning of the code is actually the opposite of the IF macro
 * (e.g. opcode 0x75 is "jump short if not equal") because the IF is
 * implemented as a jump over the body when the condition is *not*
 * fulfilled. So for instance if you have code like:
 *
 *   IF_EQUAL {
 *       do_conditional_stuff();
 *   }
 *   do_more_stuff();
 *
 * It gets recompiled like:
 *
 *     jne   skip
 *     call  do_conditional_stuff
 *   skip:
 *     call  do_more_stuff
 */
#define OP_IF_OVERFLOW      0x71
#define OP_IF_LESS_THAN     0x73
#define OP_IF_NOT_EQUAL     0x74
#define OP_IF_EQUAL         0x75
#define OP_IF_LESS_EQUAL    0x77
#define OP_IF_GREATER_EQUAL 0x7c

#define IF_OVERFLOW      IF(OP_IF_OVERFLOW)
#define IF_LESS_THAN     IF(OP_IF_LESS_THAN)
#define IF_NOT_EQUAL     IF(OP_IF_NOT_EQUAL)
#define IF_EQUAL         IF(OP_IF_EQUAL)
#define IF_LESS_EQUAL    IF(OP_IF_LESS_EQUAL)
#define IF_GREATER_EQUAL IF(OP_IF_GREATER_EQUAL)

/* 64bit "REX" prefix used to specify extended registers among other
   things. See the "Intel 64 and IA-32 Architecture Software
   Developer's Manual Volume 2A" section 2.2.1 */
static void emit_rex_prefix(struct dynarec_compiler *compiler,
                            enum X86_REG base,
                            enum X86_REG modr_m,
                            enum X86_REG index) {
   uint8_t rex = 0;

   rex |= (modr_m >= 8) << 2; /* R */
   rex |= (index >= 8)  << 1; /* X */
   rex |= (base >= 8)   << 0; /* B */

   if (rex) {
      *(compiler->map++) = rex | 0x40;
   }
}

/* Same thing as emit_rex_prefix but set the "W" bit to set 64bit
   operand size */
static void emit_rex_prefix_64(struct dynarec_compiler *compiler,
                               enum X86_REG base,
                               enum X86_REG modr_m,
                               enum X86_REG index) {
   uint8_t rex = 0x8; /* W */

   rex |= (modr_m >= 8) << 2; /* R */
   rex |= (index >= 8)  << 1; /* X */
   rex |= (base >= 8)   << 0; /* B */

   *(compiler->map++) = rex | 0x40;
}

static void emit_imm32(struct dynarec_compiler *compiler,
                        int32_t val) {
   int i;

   for (i = 0; i < 4; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
}

/* Return true if the variable fits in a signed 32bit value */
static int is_imms32(int64_t v) {
   return v >= -0x80000000L && v <= 0x7fffffffL;
}

static void emit_imm8(struct dynarec_compiler *compiler,
                      uint8_t val) {
   *(compiler->map++) = val;
}

/* Return true if the variable fits in a signed 8bit value (many
   instructions have shorter encodins for 8bit litterals) */
static int is_imms8(uint32_t v) {
   int32_t s = v;

   return s <= 0x7f && s >= -0x80;
}

static void emit_imms8(struct dynarec_compiler *compiler,
                       uint32_t val) {
   assert(is_imms8(val));

   *(compiler->map++) = val & 0xff;
}

/* Offset Scale Index Base Target addressing mode encoding */
static void emit_op_osibt(struct dynarec_compiler *compiler,
                          uint8_t op,
                          uint32_t off,
                          enum X86_REG base,
                          enum X86_REG index,
                          uint32_t scale,
                          enum X86_REG target) {
   uint8_t s;

   emit_rex_prefix(compiler, base, target, index);

   *(compiler->map++) = op;

   if (off == 0) {
      s = 0x04;
   }if (is_imms8(off)) {
      s = 0x44;
   } else {
      s = 0x84;
   }

   s |= (target & 7) << 3;
   *(compiler->map++) = s;

   switch (scale) {
   case 1:
      s = 0x00;
      break;
   case 2:
      s = 0x40;
      break;
   case 4:
      s = 0x80;
      break;
   case 8:
      s = 0xc0;
      break;
   default:
      assert("Invalid multiplier" == NULL);
   }

   *(compiler->map++) = s | (base & 7) | ((index & 7) << 3);

   if (off == 0) {
      /* Nothing to do */
   } if (is_imms8(off)) {
      emit_imms8(compiler, off);
   } else {
      emit_imm32(compiler, off);
   }
}

/* XOR %reg32, %reg32 */
static void emit_clear_reg(struct dynarec_compiler *compiler,
                           enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, reg, 0);

   reg &= 7;

   *(compiler->map++) = 0x31;
   *(compiler->map++) = 0xc0 | reg | (reg << 3);
}
#define CLEAR_REG(_r) emit_clear_reg(compiler, (_r))

/* MOV $val, %reg32 */
static void emit_mov_u32_r32(struct dynarec_compiler *compiler,
                             uint32_t val,
                             enum X86_REG reg) {
   if (val == 0) {
      CLEAR_REG(reg);
   } else {
      emit_rex_prefix(compiler, reg, 0, 0);

      *(compiler->map++) = 0xb8 | (reg & 7);
      emit_imm32(compiler, val);
   }
}
#define MOV_U32_R32(_v, _r) emit_mov_u32_r32(compiler, (_v), (_r))

/* MOV $val, off(%reg64) */
static void emit_mov_u32_off_pr64(struct dynarec_compiler *compiler,
                                  uint32_t val,
                                  uint32_t off,
                                  enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0xc7;

   /* We can use a denser encoding for small offsets */
   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | (reg & 7);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | (reg & 7);
      emit_imm32(compiler, off);
   }
   emit_imm32(compiler, val);
}
#define MOV_U32_OFF_PR64(_v, _off, _r)                  \
   emit_mov_u32_off_pr64(compiler, (_v), (_off), (_r))

static void emit_mov_r32_r32(struct dynarec_compiler *compiler,
                             enum X86_REG source,
                             enum X86_REG target) {
   assert(source != target);

   emit_rex_prefix(compiler, target, source, 0);

   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xc0 | (target & 7) | ((source & 7) << 3);
}
#define MOV_R32_R32(_s, _t) emit_mov_r32_r32(compiler, (_s), (_t))

/* MOP off(%base64), %target32 */
static void emit_mop_off_pr64_r32(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   emit_rex_prefix(compiler, base, target, 0);
   target &= 7;
   base   &= 7;

   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | base | (target << 3);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | base | (target << 3);
      emit_imm32(compiler, off);
   }
}
#define MOV_OFF_PR64_R32(_off, _r1, _r2)                        \
   emit_mop_off_pr64_r32(compiler, 0x8b, (_off), (_r1), (_r2))

/* MOP %source32, off(%base64) */
static void emit_mop_r32_off_pr64(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  enum X86_REG source,
                                  uint32_t off,
                                  enum X86_REG base) {
   emit_rex_prefix(compiler, base, source, 0);
   source &= 7;
   base   &= 7;

   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | base | (source << 3);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | base | (source << 3);
      emit_imm32(compiler, off);
   }
}
#define MOV_R32_OFF_PR64(_r1, _off, _r2)                         \
   emit_mop_r32_off_pr64(compiler, 0x89, (_r1), (_off), (_r2))

/* MOP off(%base32), %target32 */
static void emit_mop_off_pr32_r32(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   *(compiler->map++) = 0x67;

   emit_mop_off_pr64_r32(compiler, op, off, base, target);
}
#define LEA_OFF_PR32_R32(_off, _r1, _r2)                        \
   emit_mop_off_pr32_r32(compiler, 0x8d, (_off), (_r1), (_r2))

/* MOP off(%base64, %index64, $scale), %target */
static void emit_mop_off_sib_r32(struct dynarec_compiler *compiler,
                                 uint8_t op,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG index,
                                 uint32_t scale,
                                 enum X86_REG target) {
   emit_op_osibt(compiler, op, off, base, index, scale, target);
}
#define LEA_OFF_SIB_R32(_o, _b, _i, _s, _t)   \
   emit_mop_off_sib_r32(compiler, 0x8d, (_o), (_b), (_i), (_s), (_t))
#define MOV_OFF_SIB_R32(_o, _b, _i, _s, _t)   \
   emit_mop_off_sib_r32(compiler, 0x8b, (_o), (_b), (_i), (_s), (_t))

/* MOV $imm32, off(%base64, %index64, $scale) */
static void emit_mov_u32_off_sib(struct dynarec_compiler *compiler,
                                 uint32_t val,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG index,
                                 uint32_t scale) {
   emit_op_osibt(compiler, 0xc7, off, base, index, scale, 0);

   emit_imm32(compiler, val);
}
#define MOV_U32_OFF_SIB(_v, _o, _b, _i, _s)                     \
   emit_mov_u32_off_sib(compiler, (_v), (_o), (_b), (_i), (_s))

/* MOV %r32, (%target64) */
static void emit_mov_r32_pr64(struct dynarec_compiler *compiler,
                              enum X86_REG val,
                              enum X86_REG target) {

   emit_rex_prefix(compiler, target, val, 0);
   *(compiler->map++) = 0x89;
   *(compiler->map++) = (target & 7) | ((val & 7) << 3);
}
#define MOV_R32_PR64(_v, _t) emit_mov_r32_pr64(compiler, (_v), (_t))

/* MOV %r16, (%target64) */
static void emit_mov_r16_pr64(struct dynarec_compiler *compiler,
                              enum X86_REG val,
                              enum X86_REG target) {

   /* Operand-size prefix to force the move to use 16bit operands. x86
      is a marvel. Note that the REX prefix follows the operand-size
      prefix. */
   *(compiler->map++) = 0x66;

   emit_mov_r32_pr64(compiler, val, target);
}
#define MOV_R16_PR64(_v, _t) emit_mov_r16_pr64(compiler, (_v), (_t))

/* MOV %r8, (%target64) */
static void emit_mov_r8_pr64(struct dynarec_compiler *compiler,
                              enum X86_REG val,
                              enum X86_REG target) {

   emit_rex_prefix(compiler, target, val, 0);
   *(compiler->map++) = 0x88;
   *(compiler->map++) = (target & 7) | ((val & 7) << 3);
}
#define MOV_R8_PR64(_v, _t) emit_mov_r8_pr64(compiler, (_v), (_t))

/* MOV (%target64), %r32 */
static void emit_mov_pr64_r32(struct dynarec_compiler *compiler,
                              enum X86_REG addr,
                              enum X86_REG target) {

   emit_rex_prefix(compiler, addr, target, 0);
   *(compiler->map++) = 0x8b;
   *(compiler->map++) = (addr & 7) | ((target & 7) << 3);
}
#define MOV_PR64_R32(_a, _t) emit_mov_pr64_r32(compiler, (_a), (_t))

/* MOV $imm8, off(%base64, %index64, $scale) */
static void emit_mov_u8_off_sib(struct dynarec_compiler *compiler,
                                uint8_t val,
                                uint32_t off,
                                enum X86_REG base,
                                enum X86_REG index,
                                uint32_t scale) {
   emit_op_osibt(compiler, 0xc6, off, base, index, scale, 0);

   emit_imm8(compiler, val);
}
#define MOV_U8_OFF_SIB(_v, _o, _b, _i, _s)                      \
   emit_mov_u8_off_sib(compiler, (_v), (_o), (_b), (_i), (_s))

/* PUSH %reg64 */
static void emit_push_r64(struct dynarec_compiler *compiler,
                          enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0x50 | (reg & 7);
}
#define PUSH_R64(_r) emit_push_r64(compiler, (_r))

/* POP %reg64 */
static void emit_pop_r64(struct dynarec_compiler *compiler,
                         enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0x58 | (reg & 7);
}
#define POP_R64(_r) emit_pop_r64(compiler, (_r))

/* SETB %reg8 */
static void emit_setb(struct dynarec_compiler *compiler,
                      enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   reg &= 7;

   *(compiler->map++) = 0x0f;
   *(compiler->map++) = 0x92;
   *(compiler->map++) = 0xc0 + reg;
}
#define SETB_R8(_r) emit_setb(compiler, (_r))

/******************
 * ALU operations *
 ******************/

/* ALU $val, %reg32 */
static void emit_alu_u32_r32(struct dynarec_compiler *compiler,
                             uint8_t op,
                             uint32_t val,
                             enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   if (is_imms8(val)) {
      *(compiler->map++) = 0x83;
      *(compiler->map++) = op | (reg & 7);
      emit_imms8(compiler, val);
   } else {
      if (reg == REG_AX) {
         /* Operations targetting %eax have a shorter encoding */
         *(compiler->map++) = op - 0xbb;
      } else {
         *(compiler->map++) = 0x81;
         *(compiler->map++) = op | (reg & 7);
      }
      emit_imm32(compiler, val);
   }
}
#define ADD_U32_R32(_v, _r) emit_alu_u32_r32(compiler, 0xc0, (_v), (_r))
#define OR_U32_R32(_v, _r)  emit_alu_u32_r32(compiler, 0xc8, (_v), (_r))
#define AND_U32_R32(_v, _r) emit_alu_u32_r32(compiler, 0xe0, (_v), (_r))
#define SUB_U32_R32(_v, _r) emit_alu_u32_r32(compiler, 0xe8, (_v), (_r))
#define XOR_U32_R32(_v, _r) emit_alu_u32_r32(compiler, 0xf0, (_v), (_r))
#define CMP_U32_R32(_v, _r) emit_alu_u32_r32(compiler, 0xf8, (_v), (_r))

/* ALU %reg32, %reg32 */
static void emit_alu_r32_r32(struct dynarec_compiler *compiler,
                             uint8_t op,
                             enum X86_REG op0,
                             enum X86_REG op1) {
   emit_rex_prefix(compiler, op0, op1, 0);

   op0 &= 7;
   op1 &= 7;

   *(compiler->map++) = op;
   *(compiler->map++) = 0xc0 | (op0 << 3) | op1;
}
#define ADD_R32_R32(_op0, _op1) emit_alu_r32_r32(compiler, 0x01, (_op0), (_op1))
#define OR_R32_R32(_op0, _op1)  emit_alu_r32_r32(compiler, 0x09, (_op0), (_op1))
#define AND_R32_R32(_op0, _op1) emit_alu_r32_r32(compiler, 0x21, (_op0), (_op1))
#define SUB_R32_R32(_op0, _op1) emit_alu_r32_r32(compiler, 0x29, (_op0), (_op1))
#define XOR_R32_R32(_op0, _op1) emit_alu_r32_r32(compiler, 0x31, (_op0), (_op1))
#define CMP_R32_R32(_op0, _op1) emit_alu_r32_r32(compiler, 0x39, (_op0), (_op1))

static void emit_alu_off_pr64_rx(struct dynarec_compiler *compiler,
                                 uint8_t op,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG target) {
   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | (base & 7) | ((target & 7) << 3);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | (base & 7) | ((target & 7) << 3);
      emit_imm32(compiler, off);
   }
}

/* ALU off(%base64), %target32 */
static void emit_alu_off_pr64_r32(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   emit_rex_prefix(compiler, base, target, 0);
   emit_alu_off_pr64_rx(compiler, op, off, base, target);
}
#define ADD_OFF_PR64_R32(_o, _b, _t)                            \
   emit_alu_off_pr64_r32(compiler, 0x03, (_o), (_b), (_t))
#define AND_OFF_PR64_R32(_o, _b, _t)                            \
   emit_alu_off_pr64_r32(compiler, 0x23, (_o), (_b), (_t))
#define CMP_OFF_PR64_R32(_o, _b, _t)                            \
   emit_alu_off_pr64_r32(compiler, 0x3b, (_o), (_b), (_t))

/* ALU off(%base64), %target64 */
static void emit_alu_off_pr64_r64(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   emit_rex_prefix_64(compiler, base, target, 0);
   emit_alu_off_pr64_rx(compiler, op, off, base, target);
}
#define ADD_OFF_PR64_R64(_o, _b, _t)                            \
   emit_alu_off_pr64_r64(compiler, 0x03, (_o), (_b), (_t))
#define AND_OFF_PR64_R64(_o, _b, _t)                            \
   emit_alu_off_pr64_r64(compiler, 0x23, (_o), (_b), (_t))

/* The encoding of the reciprocal is identical with only a bit flip in
   the opcode. */

/* ALU %reg32, off(%base64) */
#define ADD_R32_OFF_PR64(_r, _o, _b)                            \
   emit_alu_off_pr64_r32(compiler, 0x01, (_o), (_b), (_r))
#define CMP_R32_OFF_PR64(_r, _o, _b)                            \
   emit_alu_off_pr64_r32(compiler, 0x39, (_o), (_b), (_r))

/* ALU %reg64, off(%base64) */
#define ADD_R64_OFF_PR64(_r, _o, _b)                            \
   emit_alu_off_pr64_r64(compiler, 0x01, (_o), (_b), (_r))
#define CMP_R64_OFF_PR64(_r, _o, _b)                            \
   emit_alu_off_pr64_r64(compiler, 0x39, (_o), (_b), (_r))

/* ALU $u32, off(%base64) */
static void emit_alu_u32_off_pr64(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t v,
                                  uint32_t off,
                                  enum X86_REG base) {
   emit_rex_prefix(compiler, base, 0, 0);

   base &= 7;

   if (is_imms8(v)) {
      *(compiler->map++) = 0x83;
   } else {
      *(compiler->map++) = 0x81;
   }

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | op | base;
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | op | base;
      emit_imm32(compiler, off);
   }

   if (is_imms8(v)) {
      emit_imms8(compiler, v);
   } else {
      emit_imm32(compiler, v);
   }
}
#define ADD_U32_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u32_off_pr64(compiler, 0x00, (_v), (_o), (_b))
#define OR_U32_OFF_PR64(_v, _o, _b)                             \
   emit_alu_u32_off_pr64(compiler, 0x08, (_v), (_o), (_b))
#define AND_U32_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u32_off_pr64(compiler, 0x20, (_v), (_o), (_b))
#define CMP_U32_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u32_off_pr64(compiler, 0x38, (_v), (_o), (_b))

/* ALU $u8, off(%base64) */
static void emit_alu_u8_off_pr64(struct dynarec_compiler *compiler,
                                 uint8_t op,
                                 uint8_t v,
                                 uint32_t off,
                                 enum X86_REG base) {
   emit_rex_prefix(compiler, base, 0, 0);

   base &= 7;

   *(compiler->map++) = 0x80;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | op | base;
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | op | base;
      emit_imm32(compiler, off);
   }

   emit_imm8(compiler, v);
}
#define ADD_U8_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u8_off_pr64(compiler, 0x00, (_v), (_o), (_b))
#define OR_U8_OFF_PR64(_v, _o, _b)                             \
   emit_alu_u8_off_pr64(compiler, 0x08, (_v), (_o), (_b))
#define AND_U8_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u8_off_pr64(compiler, 0x20, (_v), (_o), (_b))
#define CMP_U8_OFF_PR64(_v, _o, _b)                            \
   emit_alu_u8_off_pr64(compiler, 0x38, (_v), (_o), (_b))

/* ALU off(%b64, %i64, $s), %target32 */
static void emit_alu_off_sib_r32(struct dynarec_compiler *compiler,
                                 uint8_t op,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG index,
                                 uint32_t scale,
                                 enum X86_REG target) {
   emit_op_osibt(compiler, op, off, base, index, scale, target);
}
#define ADD_OFF_SIB_R32(_o, _b, _i, _s, _t)                             \
   emit_alu_off_sib_r32(compiler, 0x03, (_o), (_b), (_i), (_s), (_t))
#define AND_OFF_SIB_R32(_o, _b, _i, _s, _t)                             \
   emit_alu_off_sib_r32(compiler, 0x23, (_o), (_b), (_i), (_s), (_t))

#define SHL_OP      0x00U
#define SHR_OP      0x08U
#define SAR_OP      0x18U

/* SHIFT $shift, %reg32 */
static void emit_shift_u8_r32(struct dynarec_compiler *compiler,
                              uint8_t op,
                              uint8_t shift,
                              enum X86_REG reg) {
   assert(shift < 32);

   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0xc1;
   *(compiler->map++) = 0xe0 | op | (reg & 7);
   *(compiler->map++) = shift & 0x1f;
}
#define SHIFT_U8_R32(_op, _u, _v) \
   emit_shift_u8_r32(compiler, (_op), (_u), (_v))
#define SHL_U8_R32(_u, _v) emit_shift_u8_r32(compiler, SHL_OP, (_u), (_v))
#define SHR_U8_R32(_u, _v) emit_shift_u8_r32(compiler, SHR_OP, (_u), (_v))
#define SAR_U8_R32(_u, _v) emit_shift_u8_r32(compiler, SAR_OP, (_u), (_v))

/* SHIFT $shift, off(%reg64) */
static void emit_shift_u8_off_pr64(struct dynarec_compiler *compiler,
                                   uint8_t op,
                                   uint8_t shift,
                                   uint32_t off,
                                   enum X86_REG base) {
   assert(shift < 32);

   emit_rex_prefix(compiler, base, 0, 0);
   *(compiler->map++) = 0xc1;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x60 | op | (base & 7);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0xa0 | op | (base & 7);
      emit_imm32(compiler, off);
   }
}
#define SHIFT_U8_OFF_PR64(_op, _u, _o, _r) \
   emit_shift_u8_off_pr64(compiler, (_op), (_u), (_o), (_r))
#define SHL_U8_OFF_PR64(_u, _o, _r) \
   emit_shift_u8_off_pr64(compiler, SHL_OP, (_u), (_o), (_r))
#define SHR_U8_OFF_PR64(_u, _o, _r) \
   emit_shift_u8_off_pr64(compiler, SHR_OP, (_u), (_o), (_r))
#define SAR_U8_OFF_PR64(_u, _o, _r) \
   emit_shift_u8_off_pr64(compiler, SAR_OP, (_u), (_o), (_r))

/* IMUL $a, %b32, %target32 */
static void emit_imul_u32_r32_r32(struct dynarec_compiler *compiler,
                                  uint32_t a,
                                  enum X86_REG b,
                                  enum X86_REG target) {
   emit_rex_prefix(compiler, b, target, 0);

   if (is_imms8(a)) {
      *(compiler->map++) = 0x6b;
      *(compiler->map++) = 0xc0 | (b & 7) | ((target & 7) << 3);
      emit_imms8(compiler, a);
   } else {
      *(compiler->map++) = 0x69;
      *(compiler->map++) = 0xc0 | (b & 7) | ((target & 7) << 3);
      emit_imm32(compiler, a);
   }
}
#define IMUL_U32_R32_R32(_a, _b, _t)                    \
   emit_imul_u32_r32_r32(compiler, (_a), (_b), (_t))

/* JMP off. Offset is from the address of this instruction (so off = 0
   points at this jump) */
static void emit_jmp_off(struct dynarec_compiler *compiler,
                         uint32_t off) {
   if (is_imms8(off - 2)) {
      *(compiler->map++) = 0xeb;
      emit_imms8(compiler, off - 2);
   } else {
      *(compiler->map++) = 0xe9;
      emit_imm32(compiler, off - 5);
   }
}
#define EMIT_JMP(_o) emit_jmp_off(compiler, (_o))

/* JMP *off(%reg64) */
static void emit_jmp_off_pr64(struct dynarec_compiler *compiler,
                              uint32_t off,
                              enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);
   *(compiler->map++) = 0xff;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x60 | (reg & 7);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0xa0 | (reg & 7);
      emit_imm32(compiler, off);
   }
}
#define JMP_OFF_PR64(_o, _r) emit_jmp_off_pr64(compiler, (_o), (_r))

/* CALL *off(%reg64) */
static void emit_call_off_pr64(struct dynarec_compiler *compiler,
                               uint32_t off,
                               enum X86_REG reg) {
   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0xff;
   if (is_imms8(off)) {
      *(compiler->map++) = 0x50 | (reg & 7);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x90 | (reg & 7);
      emit_imm32(compiler, off);
   }
}
#define CALL_OFF_PR64(_o, _r) emit_call_off_pr64(compiler, (_o), (_r))

static void emit_call(struct dynarec_compiler *compiler,
                      dynarec_fn_t fn) {
   uint8_t *target = (void*)fn;
   intptr_t offset = target - compiler->map;

   assert(is_imms32(offset));

   /* Offset is relative to the end of the instruction */
   offset -= 5;

   *(compiler->map++) = 0xe8;
   emit_imm32(compiler, offset);
}
#define CALL(_fn) emit_call(compiler, (dynarec_fn_t)_fn)

#define MOVE_TO_BANKED(_host_reg, _psx_reg)             \
   MOV_R32_OFF_PR64(_host_reg,                          \
                    DYNAREC_STATE_REG_OFFSET(_psx_reg), \
                    STATE_REG);

#define MOVE_FROM_BANKED(_psx_reg, _host_reg)           \
   MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(_psx_reg), \
                    STATE_REG,                          \
                    _host_reg);                         \

void dynasm_emit_exception(struct dynarec_compiler *compiler,
                           enum PSX_CPU_EXCEPTION exception) {
   MOV_U32_R32(exception, REG_SI);
   MOV_U32_R32(compiler->pc, REG_DX);
   CALL(dynabi_exception);
}

void dynasm_counter_maintenance(struct dynarec_compiler *compiler,
                                unsigned cycles) {
   SUB_U32_R32(cycles, REG_CX);
}

/************************
 * Opcode recompilation *
 ************************/

void dynasm_emit_mov(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_source) {
   const int target = register_location(reg_target);
   const int source = register_location(reg_source);

   /* Moving to R0 is a NOP */
   assert(reg_target != 0);
   /* Moving from R0 is better optimized as an LI with 0 */
   assert(reg_source != 0);

   if (target >= 0) {
      if (source >= 0) {
         MOV_R32_R32(source, target);
      } else {
         MOVE_FROM_BANKED(reg_source, target);
      }
   } else {
      if (source >= 0) {
         MOV_R32_OFF_PR64(source,
                          DYNAREC_STATE_REG_OFFSET(reg_target),
                          STATE_REG);
      } else {
         /* Both registers are in memory, use EAX as temporary value */
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_source),
                          STATE_REG,
                          REG_AX);

         MOV_R32_OFF_PR64(REG_AX,
                          DYNAREC_STATE_REG_OFFSET(reg_target),
                          STATE_REG);
      }
   }
}

static void dynasm_emit_shift(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_target,
                              enum PSX_REG reg_source,
                              uint8_t shift,
                              uint8_t opcode) {
   const int target = register_location(reg_target);
   const int source = register_location(reg_source);

   if (reg_target == reg_source) {
      if (target >= 0) {
         SHIFT_U8_R32(opcode, shift, target);
      } else {
         SHIFT_U8_OFF_PR64(opcode,
                           shift,
                           DYNAREC_STATE_REG_OFFSET(reg_target),
                           STATE_REG);
      }
   } else {
      int target_tmp;

      if (target >= 0) {
         target_tmp = target;
      } else {
         /* use EAX as intermediary */
         target_tmp = REG_AX;
      }

      if (source >= 0) {
         MOV_R32_R32(source, target_tmp);
      } else {
         MOVE_FROM_BANKED(reg_source, target_tmp);
      }

      SHIFT_U8_R32(opcode, shift, target_tmp);

      if (target_tmp != target) {
         MOVE_TO_BANKED(target_tmp, reg_target);
      }
   }
}

extern void dynasm_emit_sll(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_source,
                            uint8_t shift) {
   dynasm_emit_shift(compiler, reg_target, reg_source, shift, SHL_OP);
}

extern void dynasm_emit_sra(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_source,
                            uint8_t shift) {
   dynasm_emit_shift(compiler, reg_target, reg_source, shift, SAR_OP);
}

void dynasm_emit_li(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_t,
                    uint32_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      if (val > 0) {
         MOV_U32_R32(val, target);
      } else {
         CLEAR_REG(target);
      }
   } else {
      MOV_U32_OFF_PR64(val,
                       DYNAREC_STATE_REG_OFFSET(reg_t),
                       STATE_REG);
   }
}

void dynasm_emit_addiu(struct dynarec_compiler *compiler,
                       enum PSX_REG reg_t,
                       enum PSX_REG reg_s,
                       uint32_t val) {
   int target = register_location(reg_t);
   int source = register_location(reg_s);

   if (reg_t == reg_s) {
      /* We add the register to itself */
      if (target < 0) {
         ADD_U32_OFF_PR64(val,
                          DYNAREC_STATE_REG_OFFSET(reg_t),
                          STATE_REG);
      } else {
         ADD_U32_R32(val, target);
      }
   } else {
      if (target < 0) {
         /* Use SI as intermediary */
         target = REG_SI;
      }

      if (source < 0) {
         MOVE_FROM_BANKED(reg_s, target);
      } else {
         MOV_R32_R32(source, target);
      }

      ADD_U32_R32(val, target);

      if (target == REG_SI) {
         MOVE_TO_BANKED(target, reg_t);
      }
   }
}

void dynasm_emit_addi(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_t,
                      enum PSX_REG reg_s,
                      uint32_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   /* Add to EAX (the target register shouldn't be modified in case of
      an overflow) */
   if (source >= 0) {
      MOV_R32_R32(source, REG_AX);
   } else {
      MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_s),
                       STATE_REG,
                       REG_AX);
   }

   ADD_U32_R32(val, REG_AX);

   IF_OVERFLOW {
      dynasm_emit_exception(compiler, PSX_OVERFLOW);
   } ENDIF;

   if (target >= 0) {
      MOV_R32_R32(REG_AX, target);
   } else if (reg_t != PSX_REG_R0) {
      MOV_R32_OFF_PR64(REG_AX,
                       DYNAREC_STATE_REG_OFFSET(reg_t),
                       STATE_REG);
   }
}

void dynasm_emit_addu(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_op0,
                      enum PSX_REG reg_op1) {
   const int target = register_location(reg_target);
   int r0 = register_location(reg_op0);
   int r1 = register_location(reg_op1);

   if (r0 < 0) {
      MOVE_FROM_BANKED(reg_op0, REG_AX);
      r0 = REG_AX;

      if (reg_op1 == reg_op0) {
         r1 = REG_AX;
      } else if (r1 < 0) {
         MOVE_FROM_BANKED(reg_op1, REG_SI);
         r1 = REG_SI;
      }
   }

   // Add using LEA
   if (target >= 0) {
      LEA_OFF_SIB_R32(0, r0, r1, 1, target);
   } else {
      LEA_OFF_SIB_R32(0, r0, r0, 1, REG_AX);
      MOVE_TO_BANKED(REG_AX, target);
   }
}

void dynasm_emit_or(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_target,
                    enum PSX_REG reg_op0,
                    enum PSX_REG reg_op1) {
   UNIMPLEMENTED;
}

void dynasm_emit_ori(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_t,
                     enum PSX_REG reg_s,
                     uint32_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (reg_t == reg_s) {
      /* Shortcut when we're and'ing a register with itself */
      if (target >= 0) {
         OR_U32_R32(val, target);
      } else {
         OR_U32_OFF_PR64(val,
                         DYNAREC_STATE_REG_OFFSET(reg_t),
                         STATE_REG);
      }
   } else {
      int tmp_target;

      if (target >= 0) {
         tmp_target = target;
      } else {
         tmp_target = REG_AX;
      }

      if (source >= 0) {
         MOV_R32_R32(source, tmp_target);
      } else {
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_s),
                          STATE_REG,
                          tmp_target);
      }

      OR_U32_R32(val, tmp_target);

      if (target != tmp_target) {
         MOV_R32_OFF_PR64(tmp_target,
                          DYNAREC_STATE_REG_OFFSET(reg_t),
                          STATE_REG);
      }
   }
}

void dynasm_emit_and(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_a,
                     enum PSX_REG reg_b) {
   const int a = register_location(reg_a);
   const int b = register_location(reg_b);
   const int target = register_location(reg_target);
   int t;

   assert(reg_target != reg_a || reg_target != reg_b);

   if (reg_target == reg_b) {
      // If we're and'ing with ourselves put the other register in
      // `reg_b`.
      reg_b = reg_a;
      reg_a = reg_target;
   }

   if (target > 0) {
      t = target;
   } else {
      // Use AX as temporary
      t = REG_AX;
   }

   if (reg_target != reg_a) {
      if (a > 0) {
         MOV_R32_R32(a, t);
      } else {
         MOVE_FROM_BANKED(reg_a, t);
      }
   }

   if (b > 0) {
      AND_R32_R32(reg_b, reg_target);
   } else {
      AND_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_b),
                       STATE_REG,
                       t);
   }

   if (t != target) {
      MOVE_TO_BANKED(t, reg_target);
   }
}

void dynasm_emit_andi(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_t,
                     enum PSX_REG reg_s,
                     uint32_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (reg_t == reg_s) {
      /* Shortcut when we're and'ing a register with itself */
      if (target >= 0) {
         AND_U32_R32(val, target);
      } else {
         AND_U32_OFF_PR64(val,
                          DYNAREC_STATE_REG_OFFSET(reg_t),
                          STATE_REG);
      }
   } else {
      int tmp_target;

      if (target >= 0) {
         tmp_target = target;
      } else {
         tmp_target = REG_AX;
      }

      if (source >= 0) {
         MOV_R32_R32(source, tmp_target);
      } else {
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_s),
                          STATE_REG,
                          tmp_target);
      }

      AND_U32_R32(val, tmp_target);

      if (target != tmp_target) {
         MOVE_TO_BANKED(tmp_target, reg_t);
      }
   }
}

extern void dynasm_emit_sltu(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_REG reg_op0,
                             enum PSX_REG reg_op1) {
   int target = register_location(reg_target);
   int op0 = register_location(reg_op0);
   int op1 = register_location(reg_op1);

   if (target < 0) {
      /* Use AX as temporary */
      target = REG_AX;
   }

   if (op0 < 0) {
      /* Use SI as temporary */
      op0 = REG_SI;

      MOVE_FROM_BANKED(reg_op0, REG_SI);
   }

   if (op1 < 0) {
      /* Use DX as temporary */
      op1 = REG_DX;

      MOVE_FROM_BANKED(reg_op1, REG_DX);
   }

   CLEAR_REG(target);
   CMP_R32_R32(op1, op0);
   SETB_R8(target);

   if (target == REG_AX) {
      MOVE_TO_BANKED(target, reg_target);
   }
}

extern void dynasm_emit_sltiu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_target,
                              enum PSX_REG reg_op,
                              uint32_t val) {
   int target = register_location(reg_target);
   int op = register_location(reg_op);

   if (target < 0) {
      /* Use AX as temporary */
      target = REG_AX;
   }

   if (op < 0) {
      /* Use SI as temporary */
      op = REG_SI;

      MOVE_FROM_BANKED(reg_op, REG_SI);
   }

   CLEAR_REG(target);
   CMP_U32_R32(val, op);
   SETB_R8(target);

   if (target == REG_AX) {
      MOVE_TO_BANKED(target, reg_target);
   }

}

enum MEM_DIR {
   DIR_LOAD,
   DIR_STORE,
};

enum MEM_WIDTH {
   WIDTH_BYTE = 1,
   WIDTH_HALFWORD = 2,
   WIDTH_WORD = 4,
};

static void dynasm_emit_mem_rw(struct dynarec_compiler *compiler,
                               enum PSX_REG reg_addr,
                               int16_t offset,
                               enum PSX_REG reg_val,
                               enum MEM_DIR dir,
                               enum MEM_WIDTH width) {
   int addr_r  = register_location(reg_addr);
   int value_r = register_location(reg_val);

   /* First we load the address into %edx and we add the offset */
   if (addr_r >= 0) {
      if (offset != 0) {
         LEA_OFF_PR32_R32((int32_t)offset, addr_r, REG_DX);
      } else {
         MOV_R32_R32(addr_r, REG_DX);
      }
   } else {
      if (reg_addr == PSX_REG_R0) {
         /* XXX We could optimize this since it means that the offset
            is static. Not sure if this is common enough to be worth
            it. */
         MOV_U32_R32((int32_t)offset, REG_DX);
      } else {
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_addr),
                          STATE_REG,
                          REG_DX);
         if (offset != 0) {
            ADD_U32_R32((int32_t)offset, REG_DX);
         }
      }
   }

   if (value_r < 0) {
      /* Use %rsi as temporary register */

      if (dir == DIR_STORE) {
         /* Load value to be stored */
         if (reg_val == PSX_REG_R0) {
            CLEAR_REG(REG_SI);
         } else {
            MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_val),
                             STATE_REG,
                             REG_SI);
         }
      }

      value_r = REG_SI;
   }

   if (width != WIDTH_BYTE) {
      /* Copy address to %eax */
      MOV_R32_R32(REG_DX, REG_AX);

      /* Check alignment */
      AND_U32_R32((uint32_t)width - 1, REG_AX);

      IF_NOT_EQUAL {
         /* Address is not aligned correctly. */
         enum PSX_CPU_EXCEPTION e;

         if (dir == DIR_LOAD) {
            e = PSX_EXCEPTION_LOAD_ALIGN;
         } else {
            e = PSX_EXCEPTION_STORE_ALIGN;
         }

         dynasm_emit_exception(compiler, e);
      } ENDIF;
   }

   /* Move address to %eax */
   MOV_R32_R32(REG_DX, REG_AX);

   /* Compute offset into region_mask, i.e. addr >> 29 */
   SHR_U8_R32(29, REG_AX);

   /* Mask the address. region_mask is pointed */
   AND_OFF_SIB_R32(offsetof(struct dynarec_state, region_mask),
                   STATE_REG,
                   REG_AX,
                   4,
                   REG_DX);

   /* Test if the address is in RAM */
   CMP_U32_R32(PSX_RAM_SIZE * 4, REG_DX);

   IF_LESS_THAN {
      /* We're targetting RAM */

      /* Mask the address in case it was in one of the mirrors */
      AND_U32_R32(PSX_RAM_SIZE - 1, REG_DX);

      if (dir == DIR_STORE) {
         /* Compute page index in %eax */
         MOV_R32_R32(REG_DX, REG_AX);
         SHR_U8_R32(DYNAREC_PAGE_SIZE_SHIFT, REG_AX);

         /* Clear valid flag */
         MOV_U8_OFF_SIB(0,
                        offsetof(struct dynarec_state, page_valid),
                        STATE_REG,
                        REG_AX,
                        1);
      }

      /* Add the address of the RAM buffer in host memory */
      ADD_OFF_PR64_R64(offsetof(struct dynarec_state, ram),
                       STATE_REG,
                       REG_DX);

      switch (dir) {
      case DIR_STORE:
         switch (width) {
         case WIDTH_WORD:
            MOV_R32_PR64(value_r, REG_DX);
            break;
         case WIDTH_HALFWORD:
            MOV_R16_PR64(value_r, REG_DX);
            break;
         case WIDTH_BYTE:
            MOV_R8_PR64(value_r, REG_DX);
            break;
         }
         break;
      case DIR_LOAD:
         switch (width) {
         case WIDTH_WORD:
            MOV_PR64_R32(REG_DX, value_r);
            break;
         default:
            UNIMPLEMENTED;
         }

         /* If we were using SI as temporary register and the target
            register isn't R0 we have to store the value to the real
            register location */
         if (value_r == REG_SI && reg_val != PSX_REG_R0) {
            MOV_R32_OFF_PR64(REG_SI,
                             DYNAREC_STATE_REG_OFFSET(reg_val),
                             STATE_REG);
         }
      }

   } ELSE {
      /* Test if the address is in the scratchpad */
      MOV_R32_R32(REG_DX, REG_AX);
      SUB_U32_R32(PSX_SCRATCHPAD_BASE, REG_AX);
      CMP_U32_R32(PSX_SCRATCHPAD_SIZE, REG_AX);

      IF_LESS_THAN {
         /* We're targetting the scratchpad. This is the simplest
            case, no invalidation needed, we can store it directly in
            the scratchpad buffer */

         /* Add the address of the scratchpad buffer in host memory */
         ADD_OFF_PR64_R64(offsetof(struct dynarec_state, scratchpad),
                          STATE_REG,
                          REG_AX);

         switch (dir) {
         case DIR_STORE:
            switch (width) {
            case WIDTH_WORD:
               MOV_R32_PR64(value_r, REG_DX);
               break;
            case WIDTH_HALFWORD:
               MOV_R16_PR64(value_r, REG_DX);
               break;
            case WIDTH_BYTE:
               MOV_R8_PR64(value_r, REG_DX);
               break;
            }
            break;
         case DIR_LOAD:
            switch (width) {
            case WIDTH_WORD:
               MOV_PR64_R32(REG_DX, value_r);
               break;
            default:
               UNIMPLEMENTED;
            }

            /* If we were using SI as temporary register and the target
               register isn't R0 we have to store the value to the real
               register location */
            if (value_r == REG_SI && reg_val != PSX_REG_R0) {
               MOV_R32_OFF_PR64(REG_SI,
                                DYNAREC_STATE_REG_OFFSET(reg_val),
                                STATE_REG);
            }
         }
      } ELSE {
         /* We're accessing some device's memory, call the emulator
            code */

         /* Make sure the value is in %rsi (arg1) */
         if (value_r != REG_SI) {
            MOV_R32_R32(value_r, REG_SI);
         }

         switch (dir) {
         case DIR_STORE:
            switch (width) {
            case WIDTH_WORD:
               CALL(dynabi_device_sw);
               break;
            case WIDTH_HALFWORD:
               CALL(dynabi_device_sh);
               break;
            case WIDTH_BYTE:
               CALL(dynabi_device_sb);
               break;
            }
            break;
         case DIR_LOAD:
            switch (width) {
            case WIDTH_WORD:
               CALL(dynabi_device_lw);
               break;
            default:
               UNIMPLEMENTED;
            }
            break;
         }

      } ENDIF;
   } ENDIF;
}

void dynasm_emit_sb(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_addr,
                    int16_t offset,
                    enum PSX_REG reg_val) {
   dynasm_emit_mem_rw(compiler,
                      reg_addr,
                      offset,
                      reg_val,
                      DIR_STORE, WIDTH_BYTE);
}

void dynasm_emit_sh(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_addr,
                    int16_t offset,
                    enum PSX_REG reg_val) {
   dynasm_emit_mem_rw(compiler,
                      reg_addr,
                      offset,
                      reg_val,
                      DIR_STORE, WIDTH_HALFWORD);
}

void dynasm_emit_sw(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_addr,
                    int16_t offset,
                    enum PSX_REG reg_val) {
   dynasm_emit_mem_rw(compiler,
                      reg_addr,
                      offset,
                      reg_val,
                      DIR_STORE, WIDTH_WORD);
}

void dynasm_emit_lw(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_target,
                    int16_t offset,
                    enum PSX_REG reg_addr) {
   dynasm_emit_mem_rw(compiler,
                      reg_addr,
                      offset,
                      reg_target,
                      DIR_LOAD, WIDTH_WORD);
}

void dynasm_patch(struct dynarec_compiler *compiler, int32_t offset) {
   /* `offset` is relative to the beginning of the address while the
      instruction parameter is relative to the *end* of the
      instruction so we must offset. */
   emit_imm32(compiler, ((uint32_t)offset) - 4);
}

void dynasm_emit_page_local_jump(struct dynarec_compiler *compiler,
                                 uint8_t *dynarec_target,
                                 bool placeholder) {
   int32_t offset = dynarec_target - compiler->map;

   if (placeholder == false) {
      EMIT_JMP(offset);
   } else {
      /* We're adding placeholder code we'll patch later. */
      /* XXX for not I assume the worst case scenario and make room
         for a 32bit relative jump. Since `dynarec_target` contains
         the worst case scenario we could use it to see if we could
         use a near jump instead. */
      /* JMP off32 */
      *(compiler->map++) = 0xe9;
      /* I'm supposed to put the offset here, but I don't know what it
         is yet. I use 0x90 because it's a NOP, this way we'll be able
         to patch a shorter instruction if we want later and not run
         into any issues. */
      dynarec_prepare_patch(compiler);

      *(compiler->map++) = 0x90;
      *(compiler->map++) = 0x90;
      *(compiler->map++) = 0x90;
      *(compiler->map++) = 0x90;
   }
}

static uint8_t emit_branch_cond(struct dynarec_compiler *compiler,
                                enum PSX_REG reg_a,
                                enum PSX_REG reg_b,
                                enum DYNAREC_JUMP_COND cond) {
   int a = register_location(reg_a);
   int b = register_location(reg_b);
   uint8_t op;

   switch (cond) {
   case DYNAREC_JUMP_EQ:
      op = OP_IF_EQUAL;
      break;
   case DYNAREC_JUMP_NE:
      op = OP_IF_NOT_EQUAL;
      break;
   case DYNAREC_JUMP_GE:
      op = OP_IF_GREATER_EQUAL;
      break;
   case DYNAREC_JUMP_LT:
      op = OP_IF_LESS_THAN;
      break;
   default:
      UNIMPLEMENTED;
   }

   if (reg_a == PSX_REG_R0 || reg_b == PSX_REG_R0) {
      if (reg_b == PSX_REG_R0) {
         /* The CMP opcode doesn't allow to put an immediate as 2nd
            operand, swap them around and reverse the operation if
            necessary */
         reg_b = reg_a;
         b = a;

         /* Invert op */
         switch (cond) {
         case DYNAREC_JUMP_ALWAYS:
         case DYNAREC_JUMP_EQ:
         case DYNAREC_JUMP_NE:
            /* Nothing to do */
            break;
         case DYNAREC_JUMP_GE:
            op = OP_IF_LESS_THAN;
            break;
         case DYNAREC_JUMP_LT:
            op = OP_IF_GREATER_EQUAL;
            break;
         default:
            printf("%d\n", cond);
            UNIMPLEMENTED;
         }
      }

      if (b > 0) {
         CMP_U32_R32(0, b);
      } else {
         CMP_U32_OFF_PR64(0,
                          DYNAREC_STATE_REG_OFFSET(reg_b),
                          STATE_REG);
      }
   } else {
      /* We're comparing two "real" registers */

      if (a > 0) {
         if (b > 0) {
            CMP_R32_R32(a, b);
         } else {
            uint8_t *pre = compiler->map;
            CMP_R32_OFF_PR64(a,
                             DYNAREC_STATE_REG_OFFSET(reg_b),
                             STATE_REG);
         }
      } else {
         if (b > 0) {
            CMP_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_a),
                             STATE_REG,
                             b);
         } else {
            /* Use AX as temporary */
            MOVE_FROM_BANKED(reg_b, REG_AX);

            CMP_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_a),
                             STATE_REG,
                             REG_AX);
         }
      }
   }

   return op;
}

void dynasm_emit_page_local_jump_cond(struct dynarec_compiler *compiler,
                                      uint8_t *dynarec_target,
                                      bool placeholder,
                                      enum PSX_REG reg_a,
                                      enum PSX_REG reg_b,
                                      enum DYNAREC_JUMP_COND cond) {
   uint8_t op = emit_branch_cond(compiler, reg_a, reg_b, cond);

   IF(op) {
      dynasm_emit_page_local_jump(compiler, dynarec_target, placeholder);
   } ENDIF;
}

void dynasm_emit_long_jump_imm(struct dynarec_compiler *compiler,
                               uint32_t target) {
   int32_t target_page_index = dynarec_find_page_index(compiler->state, target);
   size_t  page_valid_off = offsetof(struct dynarec_state, page_valid);
   size_t instruction_pos;

   assert(target_page_index >= 0);

   page_valid_off += target_page_index * sizeof(uint8_t);

   CMP_U8_OFF_PR64(0, page_valid_off, STATE_REG);

   IF_EQUAL {
      // The target page is invalid, we need to recompile it.

      // Load the target page index in SI
      MOV_U32_R32(target_page_index, REG_SI);
      CALL(dynabi_recompile);
   } ENDIF;

   // If we reach this point we know that the target is valid. Let's
   // look it up in `dynarec_instructions`

   // Page address
   instruction_pos = target_page_index * DYNAREC_PAGE_INSTRUCTIONS;
   // Instruction offset
   instruction_pos += (target & (DYNAREC_PAGE_SIZE - 1)) / 4;
   // Array of void *
   instruction_pos *= sizeof(void *);
   // Add offset in struct dynarec_state1
   instruction_pos += offsetof(struct dynarec_state, dynarec_instructions);

   // Jump into the new page
   JMP_OFF_PR64(instruction_pos, STATE_REG);
}

extern void dynasm_emit_long_jump_imm_cond(struct dynarec_compiler *compiler,
                                           uint32_t target,
                                           enum PSX_REG reg_a,
                                           enum PSX_REG reg_b,
                                           enum DYNAREC_JUMP_COND cond) {
   uint8_t op = emit_branch_cond(compiler, reg_a, reg_b, cond);

   IF(op) {
      dynasm_emit_long_jump_imm(compiler, target);
   } ENDIF;
}

void dynasm_emit_mfhi(struct dynarec_compiler *compiler,
                      enum PSX_REG ret_target) {
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

void dynasm_emit_mtlo(struct dynarec_compiler *compiler,
                      enum PSX_REG ret_source) {
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

void dynasm_emit_mtc0(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_source,
                      enum PSX_COP0_REG reg_cop0) {
   int source = register_location(reg_source);

   /* Move value to SI */
   if (source >= 0) {
      MOV_R32_R32(source, REG_SI);
   } else {
      if (reg_source == 0) {
         CLEAR_REG(REG_SI);
      } else {
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_source),
                          STATE_REG,
                          REG_SI);
      }
   }

   switch (reg_cop0) {
   case PSX_COP0_SR:
      CALL(dynabi_set_cop0_sr);
      break;
   case PSX_COP0_CAUSE:
      CALL(dynabi_set_cop0_cause);
      break;
   case PSX_COP0_BPC:
   case PSX_COP0_BDA:
   case PSX_COP0_DCIC:
   case PSX_COP0_BDAM:
   case PSX_COP0_BPCM:
      /* Move COP0 register index to DX */
      MOV_U32_R32(reg_cop0, REG_DX);

      CALL(dynabi_set_cop0_misc);
      break;
   case PSX_COP0_JUMPDEST:
      /* NOP */
      break;
   default:
      UNIMPLEMENTED;
   }
}
