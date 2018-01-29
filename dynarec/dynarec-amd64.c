#include <stdio.h>
#include <stddef.h>

#include "dynarec.h"

/* AMD64 register encoding.
 *
 * PAFC = Preserved Across Function Calls, per the x86-64 ABI:
 *
 * http://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
 */
enum {
   REG_AX  = 0,  /* General accumulator */
   REG_BX  = 3,  /* PSX V0 (R2) [PAFC] */
   REG_CX  = 1,  /* PSX V1 (R3) */
   REG_DX  = 2,  /* PSX A0 (R4) */
   REG_BP  = 5,  /* PSX T0 (R8) [PAFC] */
   REG_SI  = 6,  /* PSX SP (R29) */
   REG_DI  = 7,  /* PSX RA (R31) */
   REG_SP  = 4,  /* Host stack [PAFC] */
   REG_R8  = 8,
   REG_R9  = 9,
   REG_R10 = 10,
   REG_R11 = 11,
   REG_R12 = 12, /* struct dynarec_state pointer [PAFC] */
   REG_R13 = 13, /* [PAFC] */
   REG_R14 = 14, /* [PAFC] */
   REG_R15 = 15, /* [PAFC] */
};

/* Returns the host register location for the PSX-emulated register
   `reg`. Returns -1 if no host register is allocated, in which case
   it must be accessed in memory. */
static int register_location(uint8_t reg) {
   switch (reg) {
   case PSX_REG_V0:
      return REG_BX;
   case PSX_REG_V1:
      return REG_CX;
   case PSX_REG_A0:
      return REG_DX;
   case PSX_REG_T0:
      return REG_BP;
   case PSX_REG_SP:
      return REG_SI;
   case PSX_REG_RA:
      return REG_DI;
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                       \
      printf("%s:%d not implemented\n", __func__, __LINE__);     \
      abort();                                                   \
   } while (0)


static void emit_imm32(struct dynarec_compiler *compiler,
                       uint32_t val) {
   int i;

   for (i = 0; i < 4; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
}

void dynarec_emit_mov(struct dynarec_compiler *compiler,
                      uint8_t reg_t,
                      uint8_t reg_s) {
   UNIMPLEMENTED;
}

void dynarec_emit_li(struct dynarec_compiler *compiler,
                     uint8_t reg_t,
                     uint32_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      /* MOV $imm32, $r0-7 */
      *(compiler->map++) = 0xb8 | target;
      emit_imm32(compiler, val);
   } else {
      uint32_t reg_offset;

      reg_offset = offsetof(struct dynarec_state, regs);
      reg_offset += 4 * reg_t;

      /* MOV $imm32, reg_offset(%r12) */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0xc7;
      *(compiler->map++) = 0x84;
      *(compiler->map++) = 0x24;
      emit_imm32(compiler, reg_offset);
      emit_imm32(compiler, val);
   }
}

void dynarec_emit_ori(struct dynarec_compiler *compiler,
                      uint8_t reg_t,
                      uint8_t reg_s,
                      uint16_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (target >= 0 && source >= 0) {
      if (target != source) {
         /* MOV %source, %target */
         UNIMPLEMENTED;
      }

      /* OR $imm32, %r0-7 */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc8 | target;
      emit_imm32(compiler, val);
      return;
   }

   UNIMPLEMENTED;
}

void dynarec_emit_sw(struct dynarec_compiler *compiler,
                     uint8_t reg_addr,
                     int16_t offset,
                     uint8_t reg_val) {
   UNIMPLEMENTED;
}
