#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "dynarec.h"

/* AMD64 register encoding.
 *
 * PAFC = Preserved Across Function Calls, per the x86-64 ABI:
 *
 * http://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
 */
enum X86_REG {
   REG_AX  = 0,  /* Temporary variable, return value 0 */
   REG_BX  = 3,  /* [PAFC] */
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
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                       \
      printf("%s:%d not implemented\n", __func__, __LINE__);     \
      abort();                                                   \
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
#define IF(_opcode) do {                           \
   uint8_t *_jump_patch;                           \
   *((compiler)->map++) = (_opcode);               \
   _jump_patch = (compiler)->map++;

#define ELSE {                                               \
      uint32_t _jump_off = ((compiler)->map - _jump_patch) + 1;   \
      assert(_jump_off < 128);                                    \
      *_jump_patch = _jump_off;                                   \
      /* JMP imms8 */                                             \
      *((compiler)->map++) = 0xeb;                                \
      _jump_patch = (compiler)->map++;                            \
   }

#define ENDIF {                                              \
      uint32_t _jump_off = ((compiler)->map - _jump_patch) - 1;   \
      assert(_jump_off < 128);                                    \
      *_jump_patch = _jump_off;                                   \
   }} while (0)

#define IF_NOT_EQUAL     IF(0x74)
#define IF_LESS_THAN     IF(0x73)

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

/* Scale Index Base addressing mode encoding */
static void emit_sib(struct dynarec_compiler *compiler,
                     enum X86_REG base,
                     enum X86_REG index,
                     uint32_t scale) {
   uint8_t s;

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

}

void dynarec_unhandled_memory_access(struct dynarec_state *compiler,
                                     uint32_t val,
                                     uint32_t addr) {
   printf("memory access: %08x @ %08x\n", val, addr);
}

static void emit_imm32(struct dynarec_compiler *compiler,
                       uint32_t val) {
   int i;

   for (i = 0; i < 4; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
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
#define MOV_U32_OFF_PR64(_v, _off, _r) \
   emit_mov_u32_off_pr64(compiler, (_v), (_off), (_r))

static void emit_mov_r32_r32(struct dynarec_compiler *compiler,
                             enum X86_REG source,
                             enum X86_REG target) {

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

   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | (base & 7) | (target << 3);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | (base & 7) | (target << 3);
      emit_imm32(compiler, off);
   }
}
#define MOV_OFF_PR64_R32(_off, _r1, _r2) \
   emit_mop_off_pr64_r32(compiler, 0x8b, (_off), (_r1), (_r2))

/* MOP off(%base32), %target32 */
static void emit_mop_off_pr32_r32(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   *(compiler->map++) = 0x67;

   emit_mop_off_pr64_r32(compiler, op, off, base, target);
}
#define LEA_OFF_PR32_R32(_off, _r1, _r2) \
   emit_mop_off_pr32_r32(compiler, 0x8d, (_off), (_r1), (_r2))

/* MOV $val, off(%base64, %index64, $scale) */
static void emit_mov_u32_off_sib(struct dynarec_compiler *compiler,
                                 uint32_t val,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG index,
                                 uint32_t scale) {
   emit_rex_prefix(compiler, base, 0, index);

   *(compiler->map++) = 0xc7;
   *(compiler->map++) = 0x84;
   emit_sib(compiler, base, index, scale);
   emit_imm32(compiler, off);
   emit_imm32(compiler, val);
}
#define MOV_U32_OFF_SIB(_v, _o, _b, _i, _s)                     \
   emit_mov_u32_off_sib(compiler, (_v), (_o), (_b), (_i), (_s))

/* MOV %val32, (%target64) */
static void emit_mov_r32_pr64(struct dynarec_compiler *compiler,
                              enum X86_REG val,
                              enum X86_REG target) {

   emit_rex_prefix(compiler, target, val, 0);
   *(compiler->map++) = 0x89;
   *(compiler->map++) = (target & 7) | ((val & 7) << 3);
}
#define MOV_R32_PR64(_v, _t) emit_mov_r32_pr64(compiler, (_v), (_t))

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

/* ALU off(%base64), %target32 */
static void emit_alu_off_pr64_r32(struct dynarec_compiler *compiler,
                                  uint8_t op,
                                  uint32_t off,
                                  enum X86_REG base,
                                  enum X86_REG target) {
   emit_rex_prefix(compiler, base, target, 0);
   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x40 | (base & 7) | ((target & 7) << 3);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x80 | (base & 7) | ((target & 7) << 3);
      emit_imm32(compiler, off);
   }
}

#define ADD_OFF_PR64_R32(_o, _b, _t)                            \
   emit_alu_off_pr64_r32(compiler, 0x03, (_o), (_b), (_t))
#define AND_OFF_PR64_R32(_o, _b, _t)                            \
   emit_alu_off_pr64_r32(compiler, 0x23, (_o), (_b), (_t))

/* ALU off(%b64, %i64, $s), %target32 */
static void emit_alu_off_sib_r32(struct dynarec_compiler *compiler,
                                 uint8_t op,
                                 uint32_t off,
                                 enum X86_REG base,
                                 enum X86_REG index,
                                 uint32_t scale,
                                 enum X86_REG target) {

   emit_rex_prefix(compiler, base, target, index);
   *(compiler->map++) = op;

   if (is_imms8(off)) {
      *(compiler->map++) = 0x44 | ((target & 7) << 3);
      emit_sib(compiler, base, index, scale);
      emit_imms8(compiler, off);
   } else {
      *(compiler->map++) = 0x84 | ((target & 7) << 3);
      emit_sib(compiler, base, index, scale);
      emit_imm32(compiler, off);
   }
}

#define ADD_OFF_SIB_R32(_o, _b, _i, _s, _t)             \
   emit_alu_off_sib_r32(compiler, 0x03, (_o), (_b), (_i), (_s), (_t))
#define AND_OFF_SIB_R32(_o, _b, _i, _s, _t)             \
   emit_alu_off_sib_r32(compiler, 0x23, (_o), (_b), (_i), (_s), (_t))

/* SHIFT $shift, %reg32 */
static void emit_shift_u32_r32(struct dynarec_compiler *compiler,
                               uint8_t op,
                               uint32_t shift,
                               enum X86_REG reg) {
   assert(shift < 32);

   emit_rex_prefix(compiler, reg, 0, 0);

   *(compiler->map++) = 0xc1;
   *(compiler->map++) = op | (reg & 7);
   *(compiler->map++) = shift & 0x1f;
}

#define SHL_U32_R32(_u, _v) emit_shift_u32_r32(compiler, 0xe0, (_u), (_v))
#define SHR_U32_R32(_u, _v) emit_shift_u32_r32(compiler, 0xe8, (_u), (_v))
#define SAR_U32_R32(_u, _v) emit_shift_u32_r32(compiler, 0xf8, (_u), (_v))

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

#define IMUL_U32_R32_R32(_a, _b, _t) \
   emit_imul_u32_r32_r32(compiler, (_a), (_b), (_t))

static void emit_trap(struct dynarec_compiler *compiler) {
   /* INT 3 */
   *(compiler->map++) = 0xcc;
}

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

/* This will trash AX, DX and SI. CX value is loaded using the first
   return value of the called function (I assume it returns the
   counter there). The second return value (if any) will be available
   in DX */
static void emit_emulator_call(struct dynarec_compiler *compiler,
                               uint32_t fn_offset) {
   /* Save registers that we use and are not preserved across
      calls */
   /* XXX should we save them back where they belong in
      dynarec_state instead? */
   PUSH_R64(STATE_REG);
   PUSH_R64(REG_R8);
   PUSH_R64(REG_R9);
   PUSH_R64(REG_R10);
   PUSH_R64(REG_R11);

   CALL_OFF_PR64(fn_offset, STATE_REG);

   /* Move first return value to the counter */
   MOV_R32_R32(REG_AX, REG_CX);

   POP_R64(REG_R11);
   POP_R64(REG_R10);
   POP_R64(REG_R9);
   POP_R64(REG_R8);
   POP_R64(STATE_REG);
}
#define EMULATOR_CALL(_f) \
   emit_emulator_call(compiler, offsetof(struct dynarec_state, _f))

static void emit_exception(struct dynarec_compiler *compiler,
                           enum PSX_CPU_EXCEPTIONS exception) {
   // XXX TODO
   (void)exception;
   emit_trap(compiler);
}

/************************
 * Opcode recompilation *
 ************************/

void dynarec_emit_mov(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_t,
                      enum PSX_REG reg_s) {
   UNIMPLEMENTED;
}

void dynarec_emit_li(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_t,
                     uint32_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      MOV_U32_R32(val, target);
   } else {
      MOV_U32_OFF_PR64(val,
                       DYNAREC_STATE_REG_OFFSET(reg_t),
                       STATE_REG);
   }
}

void dynarec_emit_ori(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_t,
                      enum PSX_REG reg_s,
                      uint16_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (target >= 0 && source >= 0) {
      if (target != source) {
         /* MOV %source, %target */
         UNIMPLEMENTED;
      }

      OR_U32_R32(val, target);
   } else {
      UNIMPLEMENTED;
   }
}

void dynarec_emit_sw(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_addr,
                     int16_t offset,
                     enum PSX_REG reg_val) {
   int addr_r  = register_location(reg_addr);
   int value_r = register_location(reg_val);

   /* First we load the address into %edx and we add the offset */
   if (addr_r >= 0) {
      LEA_OFF_PR32_R32((int32_t)offset, addr_r, REG_DX);
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
         ADD_U32_R32((int32_t)offset, REG_DX);
      }
   }

   if (value_r < 0) {
      /* Load value into %rsi */
      if (reg_val == PSX_REG_R0) {
         CLEAR_REG(REG_SI);
      } else {
         MOV_OFF_PR64_R32(DYNAREC_STATE_REG_OFFSET(reg_val),
                          STATE_REG,
                          REG_SI);
      }
   }

   /* Copy address to %eax */
   MOV_R32_R32(REG_DX, REG_AX);

   /* Check alignment */
   AND_U32_R32(3, REG_AX);

   IF_NOT_EQUAL {
      /* Address is not aligned correctly. */
      emit_exception(compiler, PSX_EXCEPTION_LOAD_ALIGN);
   } ENDIF;

   /* Move address to %eax */
   MOV_R32_R32(REG_DX, REG_AX);

   /* Compute offset into region_mask, i.e. addr >> 29 */
   SHR_U32_R32(29, REG_AX);

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

      /* Compute page index in %eax */
      MOV_R32_R32(REG_DX, REG_AX);
      SHR_U32_R32(DYNAREC_PAGE_SIZE_SHIFT, REG_AX);

      /* Compute offset in the page table */
      IMUL_U32_R32_R32(sizeof(struct dynarec_page),
                       REG_AX,
                       REG_AX);

      /* Clear valid flag */
      MOV_U32_OFF_SIB(0,
                      offsetof(struct dynarec_state, pages),
                      STATE_REG,
                      REG_AX,
                      1);

      /* Add the address of the RAM buffer in host memory */
      ADD_OFF_PR64_R32(offsetof(struct dynarec_state, ram),
                       STATE_REG,
                       REG_DX);
      if (value_r >= 0) {
         MOV_R32_PR64(value_r, REG_DX);
      } else {
         MOV_R32_PR64(REG_SI, REG_DX);
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

         /* Add the address of the RAM buffer in host memory */
         ADD_OFF_PR64_R32(offsetof(struct dynarec_state, scratchpad),
                          STATE_REG,
                          REG_AX);

         if (value_r >= 0) {
            MOV_R32_PR64(value_r, REG_AX);
         } else {
            MOV_R32_PR64(REG_SI, REG_AX);
         }
      } ELSE {
         /* We're writing to some device's memory, call the emulator
            code */

         /* Make sure the value is in %rsi (arg1) */
         if (value_r >= 0) {
            MOV_R32_R32(value_r, REG_SI);
         }

         EMULATOR_CALL(memory_sw);
      } ENDIF;
   } ENDIF;

   /* XXX Finish me */
   emit_trap(compiler);
}
